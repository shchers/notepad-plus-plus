// NppRemote — the protocol side: one connection to one server, used from
// the worker thread only (Session), so the clients block freely.
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nppremote {

enum class Protocol { Ftp, Sftp };

struct Profile {
    std::string name;
    Protocol protocol = Protocol::Sftp;
    std::string host;
    int port = 0; // 0: the protocol's default
    std::string user;
    std::string password;     // empty: asked when the server wants one
    bool savePassword = false;
    std::string initialDir;   // empty: where the server puts us
    bool activeMode = false;  // FTP: PORT/EPRT instead of PASV/EPSV
    std::string identityFile; // SFTP: -i
    std::string sftpOptions;  // SFTP: more arguments, e.g. "-o ProxyJump=gw -F ~/.ssh/other"
    int timeout = 30;         // seconds

    int effectivePort() const { return port > 0 ? port : protocol == Protocol::Ftp ? 21 : 22; }
};

struct Entry {
    std::string name;
    bool isDir = false;
    bool isLink = false;
    std::int64_t size = -1;
    std::time_t mtime = 0; // 0: unknown
};

enum class LogKind { Info, Command, Reply, Error };

// What a client needs from the user interface. Both are called on the worker
// thread and block until answered.
struct ClientHooks {
    std::function<void(LogKind, const std::string&)> log;
    // A secret (password, passphrase, one-time code); false when cancelled.
    std::function<bool(const std::string& prompt, std::string& answer)> askSecret;
    // A yes/no question (an unknown host key).
    std::function<bool(const std::string& question)> confirm;
};

class RemoteClient {
public:
    virtual ~RemoteClient() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool connected() const = 0;
    // Makes a blocked call return (from any thread): closes the sockets or
    // stops the child process.
    virtual void abort() = 0;

    virtual bool home(std::string& path) = 0; // the directory after login
    virtual bool list(const std::string& dir, std::vector<Entry>& out) = 0;
    virtual bool download(const std::string& remote, const std::string& local) = 0;
    virtual bool upload(const std::string& local, const std::string& remote) = 0;
    virtual bool makeDir(const std::string& path) = 0;
    virtual bool removeDir(const std::string& path) = 0;
    virtual bool removeFile(const std::string& path) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;

    // Why the last call failed.
    const std::string& error() const { return error_; }
    // The last failure lost the connection (worth one reconnect and retry).
    bool connectionLost() const { return lost_; }

protected:
    bool fail(const std::string& what, bool lost = false)
    {
        error_ = what;
        lost_ = lost;
        return false;
    }
    void clearError()
    {
        error_.clear();
        lost_ = false;
    }

    std::string error_;
    bool lost_ = false;
};

std::unique_ptr<RemoteClient> makeFtpClient(const Profile& p, const ClientHooks& hooks);
std::unique_ptr<RemoteClient> makeSftpClient(const Profile& p, const ClientHooks& hooks);

inline std::unique_ptr<RemoteClient> makeClient(const Profile& p, const ClientHooks& hooks)
{
    return p.protocol == Protocol::Ftp ? makeFtpClient(p, hooks) : makeSftpClient(p, hooks);
}

} // namespace nppremote
