// NppRemote — SFTP through OpenSSH's own client, run by SshProcess. sftp
// reads its commands from a socket, so it neither echoes nor edits lines.
// Each command is followed by a comment line ("#nppremote-N"): sftp echoes
// every line it reads ("sftp> ..."), so the echo of the comment marks the end
// of the command's output, and anything on stderr by then is its error.

#include "RemoteClient.h"
#include "RemoteUtil.h"
#include "SshProcess.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

namespace nppremote {

namespace {

std::string errnoText(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

class SftpClient final : public RemoteClient {
public:
    SftpClient(const Profile& p, const ClientHooks& h) : profile_(p), hooks_(h), proc_(p, h) {}
    ~SftpClient() override { proc_.stop(true); }

    bool connect() override;
    void disconnect() override { proc_.stop(false, "bye\n"); }
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
    bool procFailed() { return fail(proc_.error(), proc_.lost()); }
    // Sends `cmd` (logged unless empty) and the marker, waits for the
    // marker's echo; `output` is what came before it.
    bool exchange(const std::string& cmd, const std::string& logged, std::string& output);
    bool run(const std::string& cmd, std::string& output);
    bool run1(const char* verb, const std::string& path)
    {
        std::string q, out;
        if (!sftpQuote(path, q))
            return fail("a name with a line break cannot be used with sftp");
        return run(std::string(verb) + " " + q, out);
    }

    Profile profile_;
    ClientHooks hooks_;
    SshProcess proc_;
    unsigned seq_ = 0;
    std::string home_;
};

bool SftpClient::exchange(const std::string& cmd, const std::string& logged, std::string& output)
{
    clearError();
    output.clear();
    if (!logged.empty())
        log(LogKind::Command, logged);
    const std::string marker = "#nppremote-" + std::to_string(++seq_);
    if (!proc_.send(cmd + "\n" + marker + "\n"))
        return procFailed();
    const std::string echo = "sftp> " + marker + "\n";
    std::string& buf = proc_.out();
    std::size_t at = std::string::npos;
    if (!proc_.pump([&] { return (at = buf.find(echo)) != std::string::npos; }))
        return procFailed();
    output = buf.substr(0, at);
    buf.erase(0, at + echo.size());
    // Drop the echo of the command itself.
    const std::string cmdEcho = "sftp> " + cmd + "\n";
    const std::size_t c = output.find(cmdEcho);
    if (c != std::string::npos)
        output.erase(0, c + cmdEcho.size());
    return true;
}

bool SftpClient::run(const std::string& cmd, std::string& output)
{
    if (!proc_.running())
        return fail("not connected", true);
    if (!exchange(cmd, cmd, output))
        return false;
    const std::string err = proc_.takeErrors();
    if (!err.empty()) {
        log(LogKind::Error, err);
        return fail(err);
    }
    return true;
}

bool SftpClient::connect()
{
    proc_.stop(true);
    clearError();
    proc_.resetAbort();
    home_.clear();
    log(LogKind::Info, "Connecting to " + profile_.host + ":" + std::to_string(profile_.effectivePort()) + " (SFTP)");
    std::vector<std::string> args = {"sftp"};
    if (!proc_.sshOptions("-P", args))
        return procFailed();
    std::string host = profile_.host;
    if (host.find(':') != std::string::npos && host.front() != '[')
        host = "[" + host + "]"; // IPv6
    args.push_back("--");
    args.push_back(profile_.user.empty() ? host : profile_.user + "@" + host);
    if (!proc_.start(args, "the OpenSSH sftp client is not installed (package openssh-client)"))
        return procFailed();
    // The first command waits for the login: ssh's questions come meanwhile.
    std::string out;
    if (!exchange("pwd", "pwd", out))
        return false;
    // ssh's notes ("Warning: Permanently added ...", "Connected to ...").
    const std::string notes = proc_.takeErrors();
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
