// NppRemote — SFTP through OpenSSH's own client. `sftp` runs with its
// commands on a socket and its output on pipes, so it neither echoes nor
// edits lines; a pseudo-terminal is its controlling terminal, which is where
// ssh asks for passwords, passphrases and unknown host keys. Each command is
// followed by a comment line ("#nppremote-N"): sftp echoes every line it
// reads ("sftp> ..."), so the echo of the comment marks the end of the
// command's output, and anything on stderr by then is its error. (GLib only
// splits the user's extra options.)

#include "RemoteClient.h"
#include "RemoteUtil.h"

#include <glib.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>

extern char** environ;

namespace nppremote {

namespace {

using Clock = std::chrono::steady_clock;

std::string errnoText(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

void closeFd(int& fd)
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

void setNonBlocking(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

void setCloseOnExec(int fd)
{
    ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

std::string findProgram(const std::string& name)
{
    const char* path = ::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/bin:/bin";
    std::size_t i = 0;
    while (i <= dirs.size()) {
        std::size_t colon = dirs.find(':', i);
        if (colon == std::string::npos)
            colon = dirs.size();
        std::string dir = dirs.substr(i, colon - i);
        i = colon + 1;
        if (dir.empty())
            dir = ".";
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return {};
}

// Without surrounding blanks, and "\n" for ssh's "\r\n".
std::string trimmed(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && (s[b] == '\r' || s[b] == '\n' || s[b] == ' '))
        ++b;
    return s.substr(b);
}

// ptsname() is not reentrant.
std::mutex ptsMutex;

class SftpClient final : public RemoteClient {
public:
    SftpClient(const Profile& p, const ClientHooks& h) : profile_(p), hooks_(h), password_(p.password) {}
    ~SftpClient() override { stop(true); }

    bool connect() override;
    void disconnect() override { stop(false); }
    bool connected() const override { return pid_ > 0; }
    void abort() override
    {
        aborted_ = true;
        const pid_t pid = pid_;
        if (pid > 0)
            ::kill(pid, SIGTERM);
    }

    bool home(std::string& path) override
    {
        path = home_;
        return !home_.empty();
    }
    bool list(const std::string& dir, std::vector<Entry>& out) override;
    bool download(const std::string& remote, const std::string& local) override;
    bool upload(const std::string& local, const std::string& remote) override;
    bool makeDir(const std::string& path) override { return run1("mkdir", path); }
    bool removeDir(const std::string& path) override { return run1("rmdir", path); }
    bool removeFile(const std::string& path) override { return run1("rm", path); }
    bool rename(const std::string& from, const std::string& to) override
    {
        std::string a, b, out;
        if (!sftpQuote(from, a) || !sftpQuote(to, b))
            return fail("a name with a line break cannot be used with sftp");
        return run("rename " + a + " " + b, out);
    }

private:
    void log(LogKind k, const std::string& s) const
    {
        if (hooks_.log)
            hooks_.log(k, s);
    }

    bool spawn();
    void stop(bool quick);
    // Reads what the child writes, answers ssh's questions on the terminal,
    // until `done` says so (true), the deadline passes or the child ends.
    template <typename Done>
    bool pump(Done done);
    void handleTerminal();
    bool run(const std::string& cmd, std::string& output);
    bool run1(const char* verb, const std::string& path)
    {
        std::string q, out;
        if (!sftpQuote(path, q))
            return fail("a name with a line break cannot be used with sftp");
        return run(std::string(verb) + " " + q, out);
    }
    bool childGone(const std::string& fallback);

    Profile profile_;
    ClientHooks hooks_;
    std::string password_;
    bool passwordTried_ = false;
    std::atomic<pid_t> pid_{0};
    std::atomic<bool> aborted_{false};
    int in_ = -1, out_ = -1, err_ = -1, pty_ = -1;
    std::string outBuf_, errBuf_, ptyBuf_;
    Clock::time_point deadline_;
    unsigned seq_ = 0;
    std::string home_;
};

bool SftpClient::spawn()
{
    const std::string program = findProgram("sftp");
    if (program.empty())
        return fail("the OpenSSH sftp client is not installed (package openssh-client)");

    std::vector<std::string> args = {"sftp", "-P", std::to_string(profile_.effectivePort()),
                                     "-o", "ServerAliveInterval=30",
                                     "-o", "ConnectTimeout=" + std::to_string(profile_.timeout)};
    if (!profile_.identityFile.empty()) {
        args.push_back("-i");
        args.push_back(profile_.identityFile);
    }
    // Split as a shell would ("-o 'ProxyCommand=ssh gw -W %h:%p'").
    if (!profile_.sftpOptions.empty()) {
        gchar** extra = nullptr;
        GError* err = nullptr;
        if (!g_shell_parse_argv(profile_.sftpOptions.c_str(), nullptr, &extra, &err)) {
            const std::string why = std::string("the sftp options: ") + err->message;
            g_error_free(err);
            return fail(why);
        }
        for (gchar** e = extra; *e; ++e)
            args.push_back(*e);
        g_strfreev(extra);
    }
    std::string host = profile_.host;
    if (host.find(':') != std::string::npos && host.front() != '[')
        host = "[" + host + "]"; // IPv6
    args.push_back("--");
    args.push_back(profile_.user.empty() ? host : profile_.user + "@" + host);

    // C dates and messages, UTF-8 names; questions on the terminal, never an askpass window.
    std::vector<std::string> env;
    static const char* const replaced[] = {"LC_ALL=", "LANG=", "TERM=", "SSH_ASKPASS_REQUIRE="};
    for (char** e = environ; *e; ++e) {
        bool keep = true;
        for (const char* r : replaced)
            keep = keep && std::strncmp(*e, r, std::strlen(r)) != 0;
        if (keep)
            env.push_back(*e);
    }
    env.push_back("LC_ALL=C.UTF-8");
    env.push_back("TERM=dumb");
    env.push_back("SSH_ASKPASS_REQUIRE=never");

    // Everything exec needs is built before fork: only async-signal-safe
    // calls are allowed in the child of a threaded process.
    std::vector<char*> argv, envp;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    for (std::string& e : env)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) < 0 || ::unlockpt(master) < 0) {
        const std::string why = errnoText("pseudo-terminal");
        closeFd(master);
        return fail(why);
    }
    setCloseOnExec(master);
    std::string slave;
    {
        std::lock_guard<std::mutex> lock(ptsMutex);
        const char* name = ::ptsname(master);
        slave = name ? name : "";
    }
    // Commands go through a socket: writing to it after the child died
    // fails with EPIPE (MSG_NOSIGNAL) instead of killing Notepad++ with SIGPIPE.
    int cmd[2], out[2], err[2];
    if (slave.empty() || ::socketpair(AF_UNIX, SOCK_STREAM, 0, cmd) < 0) {
        closeFd(master);
        return fail(errnoText("socketpair"));
    }
    if (::pipe(out) < 0) {
        ::close(cmd[0]), ::close(cmd[1]), closeFd(master);
        return fail(errnoText("pipe"));
    }
    if (::pipe(err) < 0) {
        ::close(cmd[0]), ::close(cmd[1]), ::close(out[0]), ::close(out[1]), closeFd(master);
        return fail(errnoText("pipe"));
    }
    for (int fd : {cmd[0], cmd[1], out[0], out[1], err[0], err[1]})
        setCloseOnExec(fd);

    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {cmd[0], cmd[1], out[0], out[1], err[0], err[1]})
            ::close(fd);
        closeFd(master);
        return fail(errnoText("fork"));
    }
    if (pid == 0) {
        // A new session; opening the terminal makes it the controlling one.
        ::setsid();
        const int tty = ::open(slave.c_str(), O_RDWR);
        if (tty < 0)
            ::_exit(126);
        termios t;
        if (::tcgetattr(tty, &t) == 0) {
            t.c_oflag &= ~static_cast<tcflag_t>(ONLCR); // plain "\n"
            ::tcsetattr(tty, TCSANOW, &t);
        }
        ::dup2(cmd[1], 0);
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        // Notepad++'s own descriptors (display, files) stay out of ssh.
        for (int fd = 3; fd < 1024; ++fd)
            ::close(fd);
        ::execve(program.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(cmd[1]);
    ::close(out[1]);
    ::close(err[1]);
    in_ = cmd[0];
    out_ = out[0];
    err_ = err[0];
    pty_ = master;
    setNonBlocking(out_);
    setNonBlocking(err_);
    setNonBlocking(pty_);
    pid_ = pid;
    outBuf_.clear();
    errBuf_.clear();
    ptyBuf_.clear();
    return true;
}

void SftpClient::stop(bool quick)
{
    const pid_t pid = pid_;
    if (pid > 0) {
        if (!quick && !aborted_ && in_ >= 0) {
            static const char bye[] = "bye\n";
            ::send(in_, bye, sizeof bye - 1, MSG_NOSIGNAL);
        }
        closeFd(in_); // end of input: sftp quits
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < (quick ? 1 : 30) && !reaped; ++i) {
            reaped = ::waitpid(pid, &status, WNOHANG) == pid;
            if (!reaped && !quick)
                ::poll(nullptr, 0, 50);
        }
        if (!reaped) {
            ::kill(pid, SIGTERM);
            for (int i = 0; i < 20 && !reaped; ++i) {
                reaped = ::waitpid(pid, &status, WNOHANG) == pid;
                if (!reaped)
                    ::poll(nullptr, 0, 25);
            }
            if (!reaped) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
            }
        }
        pid_ = 0;
    }
    closeFd(in_);
    closeFd(out_);
    closeFd(err_);
    closeFd(pty_);
}

bool SftpClient::childGone(const std::string& fallback)
{
    // What ssh said last is the reason ("Permission denied", "Connection refused").
    const std::string why = trimmed(errBuf_);
    if (!why.empty())
        log(LogKind::Error, why);
    errBuf_.clear();
    stop(true);
    return fail(aborted_ ? "cancelled" : why.empty() ? fallback : why, !aborted_);
}

void SftpClient::handleTerminal()
{
    // Questions end with ": " (passwords, passphrases, codes) or "? " (host keys).
    if (ptyBuf_.empty() || ptyBuf_.back() != ' ')
        return;
    const std::string text = trimmed(ptyBuf_);
    if (text.empty())
        return;
    const char last = text.back();
    if (last != ':' && last != '?')
        return;
    ptyBuf_.clear();
    std::string answer;
    bool ok = false;
    if (text.find("(yes/no") != std::string::npos) {
        ok = true;
        answer = hooks_.confirm && hooks_.confirm(text) ? "yes" : "no";
        log(LogKind::Info, "Host key: " + answer);
    } else {
        std::string prompt = text;
        const std::size_t nl = prompt.rfind('\n');
        if (nl != std::string::npos)
            prompt = trimmed(prompt.substr(nl + 1));
        const bool isPassword = prompt.find("assword") != std::string::npos;
        if (isPassword && !password_.empty() && !passwordTried_) {
            passwordTried_ = true;
            answer = password_;
            ok = true;
        } else {
            if (isPassword)
                password_.clear(); // the saved one was wrong
            ok = hooks_.askSecret && hooks_.askSecret(prompt, answer);
            if (ok && isPassword)
                password_ = answer;
        }
        log(LogKind::Info, prompt + (ok ? " ********" : " (cancelled)"));
    }
    if (!ok) {
        abort();
        return;
    }
    answer += '\n';
    for (std::size_t done = 0; done < answer.size();) {
        const ssize_t n = ::write(pty_, answer.data() + done, answer.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            pollfd p{pty_, POLLOUT, 0};
            ::poll(&p, 1, 1000);
            continue;
        }
        if (n <= 0)
            break;
        done += static_cast<std::size_t>(n);
    }
    // The user may have taken a while.
    deadline_ = Clock::now() + std::chrono::seconds(profile_.timeout);
}

template <typename Done>
bool SftpClient::pump(Done done)
{
    for (;;) {
        if (done())
            return true;
        if (pid_ <= 0)
            return childGone("not connected");
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - Clock::now()).count();
        if (left <= 0) {
            abort();
            aborted_ = false; // a timeout, not the user: worth a reconnect
            return childGone("the server did not answer in " + std::to_string(profile_.timeout) + " s");
        }
        pollfd p[3] = {{out_, POLLIN, 0}, {err_, POLLIN, 0}, {pty_, POLLIN, 0}};
        const int r = ::poll(p, 3, static_cast<int>(left < 1000 ? left : 1000));
        if (r < 0 && errno != EINTR)
            return childGone(errnoText("poll"));
        char chunk[16384];
        bool outEnded = false;
        if (p[0].revents) {
            const ssize_t n = ::read(out_, chunk, sizeof chunk);
            if (n > 0) {
                outBuf_.append(chunk, static_cast<std::size_t>(n));
                // A long transfer prints nothing: output is life.
                deadline_ = Clock::now() + std::chrono::seconds(profile_.timeout);
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                outEnded = true;
            }
        }
        if (p[1].revents) {
            const ssize_t n = ::read(err_, chunk, sizeof chunk);
            if (n > 0)
                errBuf_.append(chunk, static_cast<std::size_t>(n));
        }
        if (p[2].revents & POLLIN) {
            const ssize_t n = ::read(pty_, chunk, sizeof chunk);
            if (n > 0) {
                ptyBuf_.append(chunk, static_cast<std::size_t>(n));
                handleTerminal();
            }
        }
        if (outEnded) {
            // Collect the last of stderr: it says why.
            for (int i = 0; i < 20; ++i) {
                pollfd e{err_, POLLIN, 0};
                if (::poll(&e, 1, 50) <= 0)
                    break;
                const ssize_t n = ::read(err_, chunk, sizeof chunk);
                if (n <= 0)
                    break;
                errBuf_.append(chunk, static_cast<std::size_t>(n));
            }
            return childGone("the connection was closed");
        }
    }
}

bool SftpClient::run(const std::string& cmd, std::string& output)
{
    clearError();
    output.clear();
    if (pid_ <= 0)
        return fail("not connected", true);
    log(LogKind::Command, cmd);
    const std::string marker = "#nppremote-" + std::to_string(++seq_);
    const std::string text = cmd + "\n" + marker + "\n";
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::send(in_, text.data() + done, text.size() - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return childGone(errnoText("send"));
        done += static_cast<std::size_t>(n);
    }
    deadline_ = Clock::now() + std::chrono::seconds(profile_.timeout);
    const std::string echo = "sftp> " + marker + "\n";
    std::size_t at = std::string::npos;
    if (!pump([&] { return (at = outBuf_.find(echo)) != std::string::npos; }))
        return false;
    output = outBuf_.substr(0, at);
    outBuf_.erase(0, at + echo.size());
    // Drop the echo of the command itself.
    const std::string cmdEcho = "sftp> " + cmd + "\n";
    const std::size_t c = output.find(cmdEcho);
    if (c != std::string::npos)
        output.erase(0, c + cmdEcho.size());
    // sftp wrote any error before reading the marker: it is in the pipe now.
    char chunk[4096];
    for (;;) {
        const ssize_t n = ::read(err_, chunk, sizeof chunk);
        if (n <= 0)
            break;
        errBuf_.append(chunk, static_cast<std::size_t>(n));
    }
    const std::string err = trimmed(errBuf_);
    errBuf_.clear();
    if (!err.empty()) {
        log(LogKind::Error, err);
        return fail(err);
    }
    return true;
}

bool SftpClient::connect()
{
    stop(true);
    clearError();
    aborted_ = false;
    passwordTried_ = false;
    log(LogKind::Info, "Connecting to " + profile_.host + ":" + std::to_string(profile_.effectivePort()) + " (SFTP)");
    if (!spawn())
        return false;
    std::string out;
    // The first command waits for the login: ssh's questions come meanwhile.
    clearError();
    log(LogKind::Command, "pwd");
    static const char first[] = "pwd\n#nppremote-0\n";
    if (::send(in_, first, sizeof first - 1, MSG_NOSIGNAL) < 0)
        return childGone(errnoText("send"));
    deadline_ = Clock::now() + std::chrono::seconds(profile_.timeout);
    const std::string echo = "sftp> #nppremote-0\n";
    std::size_t at = std::string::npos;
    if (!pump([&] { return (at = outBuf_.find(echo)) != std::string::npos; }))
        return false;
    out = outBuf_.substr(0, at);
    outBuf_.erase(0, at + echo.size());
    // ssh's notes ("Warning: Permanently added ...", "Connected to ...").
    const std::string notes = trimmed(errBuf_);
    errBuf_.clear();
    if (!notes.empty())
        log(LogKind::Info, notes);
    static const std::string tag = "Remote working directory: ";
    const std::size_t p = out.find(tag);
    if (p != std::string::npos) {
        const std::size_t eol = out.find('\n', p);
        home_ = out.substr(p + tag.size(), eol == std::string::npos ? std::string::npos : eol - p - tag.size());
    }
    if (home_.empty())
        home_ = "/";
    log(LogKind::Reply, tag + home_);
    log(LogKind::Info, "Connected");
    return true;
}

bool SftpClient::list(const std::string& dir, std::vector<Entry>& out)
{
    out.clear();
    std::string q, text;
    if (!sftpQuote(dir, q))
        return fail("a name with a line break cannot be used with sftp");
    // From inside the folder sftp prints bare names.
    if (!run("cd " + q, text) || !run("ls -lan", text))
        return false;
    const std::time_t now = std::time(nullptr);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        Entry e;
        if (parseSftpLsLine(text.substr(pos, eol - pos), e, now))
            out.push_back(std::move(e));
        pos = eol + 1;
    }
    return true;
}

bool SftpClient::download(const std::string& remote, const std::string& local)
{
    const std::string tmp = local + ".nppremote-part";
    std::string r, l, out;
    if (!sftpQuote(remote, r) || !sftpQuote(tmp, l))
        return fail("a name with a line break cannot be used with sftp");
    if (!run("get " + r + " " + l, out)) {
        ::unlink(tmp.c_str());
        return false;
    }
    struct stat st;
    if (::stat(tmp.c_str(), &st) < 0) // sftp said nothing, yet no file
        return fail("the download failed");
    if (::rename(tmp.c_str(), local.c_str()) < 0) {
        const std::string why = errnoText(local.c_str());
        ::unlink(tmp.c_str());
        return fail(why);
    }
    return true;
}

bool SftpClient::upload(const std::string& local, const std::string& remote)
{
    std::string l, r, out;
    if (!sftpQuote(local, l) || !sftpQuote(remote, r))
        return fail("a name with a line break cannot be used with sftp");
    return run("put " + l + " " + r, out);
}

} // namespace

std::unique_ptr<RemoteClient> makeSftpClient(const Profile& p, const ClientHooks& hooks)
{
    return std::make_unique<SftpClient>(p, hooks);
}

} // namespace nppremote
