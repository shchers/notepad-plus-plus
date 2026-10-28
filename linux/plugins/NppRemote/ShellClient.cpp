// NppRemote — the shell link, as in Midnight Commander (FISH): `ssh` runs
// /bin/sh on the server, and files are listed, read and written with plain
// shell commands (ls, cat, printf, mkdir, mv, rm), so it works where the
// server has no SFTP subsystem (routers, busybox, dropbear). Run by
// SshProcess. Each command is followed by
//     printf '\n###<nonce>-N %s\n' "$?"
// whose output ends the command's output (a file read with cat as well: the
// nonce is random) and carries its status; stderr by then is its error.

#include "RemoteClient.h"
#include "RemoteUtil.h"
#include "SshProcess.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace nppremote {

namespace {

std::string errnoText(const std::string& what)
{
    return what + ": " + std::strerror(errno);
}

class ShellClient final : public RemoteClient {
public:
    ShellClient(const Profile& p, const ClientHooks& h) : profile_(p), hooks_(h), proc_(p, h)
    {
        std::random_device rd;
        char buf[32];
        std::snprintf(buf, sizeof buf, "NPPREMOTE%08x%08x", rd(), rd());
        nonce_ = buf;
    }
    ~ShellClient() override { proc_.stop(true); }

    bool connect() override;
    void disconnect() override { proc_.stop(false, "exit\n"); }
    bool connected() const override { return proc_.running(); }
    void abort() override { proc_.abort(); }

    bool home(std::string& path) override
    {
        path = home_;
        return !home_.empty();
    }
    bool list(const std::string& dir, std::vector<Entry>& out) override;
    bool download(const std::string& remote, const std::string& local) override;
    bool upload(const std::string& local, const std::string& remote) override;
    bool makeDir(const std::string& path) override { return run("mkdir " + shellQuote(path)); }
    bool removeDir(const std::string& path) override { return run("rmdir " + shellQuote(path)); }
    bool removeFile(const std::string& path) override { return run("rm " + shellQuote(path)); }
    bool rename(const std::string& from, const std::string& to) override
    {
        return run("mv -f " + shellQuote(from) + " " + shellQuote(to));
    }

private:
    void log(LogKind k, const std::string& s) const
    {
        if (hooks_.log)
            hooks_.log(k, s);
    }
    bool procFailed() { return fail(proc_.error(), proc_.lost()); }
    // Runs `script` (its first line logged as `logged`); `output` is what
    // it printed, `status` its exit status.
    bool exchange(const std::string& script, const std::string& logged, std::string& output, int& status);
    // A command that must succeed; its standard input is /dev/null, so it
    // can never eat the commands that follow.
    bool run(const std::string& cmd, std::string* output = nullptr);

    Profile profile_;
    ClientHooks hooks_;
    SshProcess proc_;
    std::string nonce_;
    unsigned seq_ = 0;
    std::string home_;
};

bool ShellClient::exchange(const std::string& script, const std::string& logged, std::string& output, int& status)
{
    clearError();
    output.clear();
    status = -1;
    if (!proc_.running())
        return fail("not connected", true);
    if (!logged.empty())
        log(LogKind::Command, logged);
    const std::string marker = "\n###" + nonce_ + "-" + std::to_string(++seq_) + " ";
    if (!proc_.send(script + "\nprintf '\\n" + marker.substr(1) + "%s\\n' \"$?\"\n"))
        return procFailed();
    std::string& buf = proc_.out();
    // Only the new tail can hold the marker: a large file is searched once.
    std::size_t from = 0, at = std::string::npos, eol = std::string::npos;
    const bool ok = proc_.pump([&] {
        if (at == std::string::npos) {
            at = buf.find(marker, from);
            if (at == std::string::npos) {
                from = buf.size() > marker.size() ? buf.size() - marker.size() : 0;
                return false;
            }
        }
        eol = buf.find('\n', at + marker.size());
        return eol != std::string::npos;
    });
    if (!ok)
        return procFailed();
    output = buf.substr(0, at);
    status = std::atoi(buf.c_str() + at + marker.size());
    buf.erase(0, eol + 1);
    return true;
}

bool ShellClient::run(const std::string& cmd, std::string* output)
{
    std::string out;
    int status = 0;
    if (!exchange(cmd + " </dev/null", cmd, out, status))
        return false;
    const std::string err = proc_.takeErrors();
    if (output)
        *output = std::move(out);
    if (status != 0) {
        const std::string why = err.empty() ? "failed (exit status " + std::to_string(status) + ")" : err;
        log(LogKind::Error, why);
        return fail(why);
    }
    if (!err.empty())
        log(LogKind::Info, err);
    return true;
}

bool ShellClient::connect()
{
    proc_.stop(true);
    clearError();
    proc_.resetAbort();
    home_.clear();
    seq_ = 0;
    log(LogKind::Info, "Connecting to " + profile_.host + ":" + std::to_string(profile_.effectivePort()) + " (shell link)");
    // -T: no terminal on the server, so nothing is echoed and data passes as is.
    std::vector<std::string> args = {"ssh", "-T", "-e", "none"};
    if (!proc_.sshOptions("-p", args))
        return procFailed();
    args.push_back("--");
    args.push_back(profile_.user.empty() ? profile_.host : profile_.user + "@" + profile_.host);
    // Whatever the login shell is, the commands are for sh.
    args.push_back("exec /bin/sh");
    if (!proc_.start(args, "the OpenSSH client is not installed (package openssh-client)"))
        return procFailed();
    // The first command waits for the login: ssh's questions come meanwhile.
    // C dates for ls, no history; the login scripts may print before it.
    std::string out;
    int status = 0;
    if (!exchange("LC_ALL=C; export LC_ALL; unset HISTFILE; pwd", "pwd", out, status))
        return false;
    // ssh's notes ("Warning: Permanently added ...") and the login scripts'.
    const std::string notes = proc_.takeErrors();
    if (!notes.empty())
        log(LogKind::Info, notes);
    out = trimmedText(out);
    const std::size_t nl = out.rfind('\n');
    home_ = nl == std::string::npos ? out : out.substr(nl + 1);
    if (home_.empty() || home_[0] != '/')
        home_ = "/";
    log(LogKind::Reply, home_);
    log(LogKind::Info, "Connected");
    return true;
}

bool ShellClient::list(const std::string& dir, std::vector<Entry>& out)
{
    out.clear();
    std::string text;
    // The trailing '/' follows a link to a folder; -n: numbers, not names.
    if (!run("ls -lan " + shellQuote(dir.empty() || dir.back() == '/' ? dir : dir + "/"), &text))
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

bool ShellClient::download(const std::string& remote, const std::string& local)
{
    std::string data;
    if (!run("cat " + shellQuote(remote), &data))
        return false;
    const std::string tmp = local + ".nppremote-part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            const std::string why = errnoText(tmp);
            ::unlink(tmp.c_str());
            return fail(why);
        }
    }
    if (::rename(tmp.c_str(), local.c_str()) < 0) {
        const std::string why = errnoText(local);
        ::unlink(tmp.c_str());
        return fail(why);
    }
    log(LogKind::Reply, std::to_string(data.size()) + " bytes");
    return true;
}

bool ShellClient::upload(const std::string& local, const std::string& remote)
{
    std::ifstream f(local, std::ios::binary);
    if (!f)
        return fail(errnoText(local));
    std::ostringstream data;
    data << f.rdbuf();
    std::string out;
    int status = 0;
    const std::string text = data.str();
    if (!exchange(shellWriteScript(text, remote), "write " + shellQuote(remote) + " (" + std::to_string(text.size()) + " bytes)", out,
                  status))
        return false;
    const std::string err = proc_.takeErrors();
    if (status != 0) {
        const std::string why = err.empty() ? "the upload failed (exit status " + std::to_string(status) + ")" : err;
        log(LogKind::Error, why);
        return fail(why);
    }
    return true;
}

} // namespace

std::unique_ptr<RemoteClient> makeShellClient(const Profile& p, const ClientHooks& hooks)
{
    return std::make_unique<ShellClient>(p, hooks);
}

} // namespace nppremote
