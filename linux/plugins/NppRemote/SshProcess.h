// NppRemote — the OpenSSH child shared by SFTP and the shell link: `sftp`,
// or `ssh` running a shell on the server. Its input is a socket and its
// output and errors are pipes, so nothing is echoed or edited; a
// pseudo-terminal is its controlling terminal, which is where ssh asks for
// passwords, passphrases and unknown host keys. Used from the worker thread
// only, except abort().
#pragma once

#include "RemoteClient.h"

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace nppremote {

class SshProcess {
public:
    SshProcess(const Profile& p, const ClientHooks& h) : profile_(p), hooks_(h), password_(p.password) {}
    ~SshProcess() { stop(true); }
    SshProcess(const SshProcess&) = delete;
    SshProcess& operator=(const SshProcess&) = delete;

    // The connection options of the profile for `sftp` (port flag "-P") or
    // `ssh` ("-p"): port, timeouts, key, the user's extra options.
    bool sshOptions(const char* portFlag, std::vector<std::string>& args);
    // Runs args[0], found on PATH. `missing` says what to install.
    bool start(std::vector<std::string> args, const std::string& missing);
    // Ends the child: `goodbye` first unless quick, then end of input,
    // SIGTERM, SIGKILL.
    void stop(bool quick, const char* goodbye = nullptr);
    // From any thread: a blocked pump() returns.
    void abort();
    void resetAbort() { aborted_ = false; }
    bool running() const { return pid_ > 0; }

    bool send(const std::string& text);
    // Reads the child's output into out(), and answers ssh's questions,
    // until `done` says so (true), the timeout passes or the child ends.
    // Any output restarts the timeout.
    bool pump(const std::function<bool()>& done);
    // stderr so far, read to the end of what is in the pipe and trimmed;
    // cleared.
    std::string takeErrors();

    std::string& out() { return outBuf_; }
    // Why the last call failed, and whether the connection is gone with it.
    const std::string& error() const { return error_; }
    bool lost() const { return lost_; }

private:
    bool fail(const std::string& what, bool lost = false)
    {
        error_ = what;
        lost_ = lost;
        return false;
    }
    void log(LogKind k, const std::string& s) const
    {
        if (hooks_.log)
            hooks_.log(k, s);
    }
    void handleTerminal();
    bool childGone(const std::string& fallback);
    void restartTimeout() { deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(profile_.timeout); }

    Profile profile_;
    ClientHooks hooks_;
    std::string password_;
    bool passwordTried_ = false;
    std::atomic<pid_t> pid_{0};
    std::atomic<bool> aborted_{false};
    int in_ = -1, out_ = -1, err_ = -1, pty_ = -1;
    std::string outBuf_, errBuf_, ptyBuf_;
    std::chrono::steady_clock::time_point deadline_;
    std::string error_;
    bool lost_ = false;
};

// "\r\n" as "\n", without surrounding blanks and line breaks.
std::string trimmedText(std::string s);

} // namespace nppremote
