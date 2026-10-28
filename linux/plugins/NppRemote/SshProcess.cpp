// NppRemote — the OpenSSH child process (see SshProcess.h). GLib only
// splits the user's extra options.

#include "SshProcess.h"

#include <glib.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
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

// ptsname() is not reentrant.
std::mutex ptsMutex;

} // namespace

std::string trimmedText(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t b = 0;
    while (b < s.size() && (s[b] == '\n' || s[b] == ' '))
        ++b;
    return s.substr(b);
}

bool SshProcess::sshOptions(const char* portFlag, std::vector<std::string>& args)
{
    args.push_back(portFlag);
    args.push_back(std::to_string(profile_.effectivePort()));
    args.insert(args.end(), {"-o", "ServerAliveInterval=30", "-o", "ConnectTimeout=" + std::to_string(profile_.timeout)});
    if (!profile_.identityFile.empty()) {
        args.push_back("-i");
        args.push_back(profile_.identityFile);
    }
    // Split as a shell would ("-o 'ProxyCommand=ssh gw -W %h:%p'").
    if (!profile_.sftpOptions.empty()) {
        gchar** extra = nullptr;
        GError* err = nullptr;
        if (!g_shell_parse_argv(profile_.sftpOptions.c_str(), nullptr, &extra, &err)) {
            const std::string why = std::string("the ssh options: ") + err->message;
            g_error_free(err);
            return fail(why);
        }
        for (gchar** e = extra; *e; ++e)
            args.push_back(*e);
        g_strfreev(extra);
    }
    return true;
}

bool SshProcess::start(std::vector<std::string> args, const std::string& missing)
{
    stop(true);
    error_.clear();
    lost_ = false;
    passwordTried_ = false;
    const std::string program = findProgram(args.front());
    if (program.empty())
        return fail(missing);

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
    setNonBlocking(in_);
    setNonBlocking(out_);
    setNonBlocking(err_);
    setNonBlocking(pty_);
    pid_ = pid;
    outBuf_.clear();
    errBuf_.clear();
    ptyBuf_.clear();
    return true;
}

void SshProcess::stop(bool quick, const char* goodbye)
{
    const pid_t pid = pid_;
    if (pid > 0) {
        if (!quick && !aborted_ && in_ >= 0 && goodbye)
            ::send(in_, goodbye, std::strlen(goodbye), MSG_NOSIGNAL);
        closeFd(in_); // end of input: the child quits
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

void SshProcess::abort()
{
    aborted_ = true;
    const pid_t pid = pid_;
    if (pid > 0)
        ::kill(pid, SIGTERM);
}

bool SshProcess::childGone(const std::string& fallback)
{
    // What ssh said last is the reason ("Permission denied", "Connection refused").
    const std::string why = trimmedText(errBuf_);
    if (!why.empty())
        log(LogKind::Error, why);
    errBuf_.clear();
    stop(true);
    return fail(aborted_ ? "cancelled" : why.empty() ? fallback : why, !aborted_);
}

void SshProcess::handleTerminal()
{
    // Questions end with ": " (passwords, passphrases, codes) or "? " (host keys).
    if (ptyBuf_.empty() || ptyBuf_.back() != ' ')
        return;
    const std::string text = trimmedText(ptyBuf_);
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
            prompt = trimmedText(prompt.substr(nl + 1));
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
    restartTimeout();
}

bool SshProcess::send(const std::string& text)
{
    error_.clear();
    lost_ = false;
    if (pid_ <= 0)
        return fail("not connected", true);
    restartTimeout();
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::send(in_, text.data() + done, text.size() - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
            restartTimeout();
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN)
            return childGone(errnoText("send"));
        // A large upload: the child's output must not fill its pipes meanwhile.
        if (aborted_)
            return childGone("cancelled");
        if (Clock::now() > deadline_) {
            abort();
            aborted_ = false;
            return childGone("the server did not take the data in " + std::to_string(profile_.timeout) + " s");
        }
        pollfd p[3] = {{in_, POLLOUT, 0}, {out_, POLLIN, 0}, {err_, POLLIN, 0}};
        ::poll(p, 3, 1000);
        char chunk[16384];
        if (p[1].revents & POLLIN) {
            const ssize_t r = ::read(out_, chunk, sizeof chunk);
            if (r > 0)
                outBuf_.append(chunk, static_cast<std::size_t>(r));
        }
        if (p[2].revents & POLLIN) {
            const ssize_t r = ::read(err_, chunk, sizeof chunk);
            if (r > 0)
                errBuf_.append(chunk, static_cast<std::size_t>(r));
        }
    }
    return true;
}

bool SshProcess::pump(const std::function<bool()>& done)
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
                restartTimeout();
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

std::string SshProcess::takeErrors()
{
    // The child wrote them before what ended the command: they are in the pipe now.
    char chunk[4096];
    for (;;) {
        const ssize_t n = err_ >= 0 ? ::read(err_, chunk, sizeof chunk) : -1;
        if (n <= 0)
            break;
        errBuf_.append(chunk, static_cast<std::size_t>(n));
    }
    std::string err = trimmedText(errBuf_);
    errBuf_.clear();
    return err;
}

} // namespace nppremote
