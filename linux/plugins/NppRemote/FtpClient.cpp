// NppRemote — FTP (RFC 959) on POSIX sockets: passive (EPSV, then PASV) or
// active (EPRT/PORT) data connections, binary transfers, MLSD listings when
// the server has them and LIST otherwise.

#include "RemoteClient.h"
#include "RemoteUtil.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace nppremote {

namespace {

std::string errnoText(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

// Waits for `events` on `fd`; false on timeout or error.
bool waitFor(int fd, short events, int timeoutSec)
{
    pollfd p{fd, events, 0};
    for (;;) {
        const int r = ::poll(&p, 1, timeoutSec * 1000);
        if (r < 0 && errno == EINTR)
            continue;
        return r > 0;
    }
}

void closeFd(int& fd)
{
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

// Connects to the first address of `host` that answers. `err` says why none did.
int connectTo(const std::string& host, int port, int timeoutSec, std::string& err)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        err = "cannot resolve " + host + ": " + ::gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) {
            err = errnoText("socket");
            continue;
        }
        // Non-blocking connect, to time it out.
        const int flags = ::fcntl(fd, F_GETFL);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            if (waitFor(fd, POLLOUT, timeoutSec)) {
                int soErr = 0;
                socklen_t len = sizeof soErr;
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len);
                errno = soErr;
                r = soErr ? -1 : 0;
            } else {
                errno = ETIMEDOUT;
            }
        }
        if (r == 0) {
            ::fcntl(fd, F_SETFL, flags);
            break;
        }
        err = "cannot connect to " + host + ":" + service + ": " + std::strerror(errno);
        closeFd(fd);
    }
    ::freeaddrinfo(res);
    return fd;
}

bool sendAll(int fd, const char* data, std::size_t size, int timeoutSec)
{
    while (size) {
        if (!waitFor(fd, POLLOUT, timeoutSec)) {
            errno = ETIMEDOUT;
            return false;
        }
        const ssize_t n = ::send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

// The numeric host of a socket address.
std::string hostOf(const sockaddr_storage& ss)
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (ss.ss_family == AF_INET)
        ::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in&>(ss).sin_addr, buf, sizeof buf);
    else if (ss.ss_family == AF_INET6)
        ::inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6&>(ss).sin6_addr, buf, sizeof buf);
    return buf;
}

int portOf(const sockaddr_storage& ss)
{
    if (ss.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in&>(ss).sin_port);
    if (ss.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6&>(ss).sin6_port);
    return 0;
}

class FtpClient final : public RemoteClient {
public:
    FtpClient(const Profile& p, const ClientHooks& h) : profile_(p), hooks_(h), password_(p.password) {}
    ~FtpClient() override { closeAll(); }

    bool connect() override;
    void disconnect() override;
    bool connected() const override { return ctrl_ >= 0; }
    void abort() override
    {
        aborted_ = true;
        const int c = ctrl_, d = data_;
        if (c >= 0)
            ::shutdown(c, SHUT_RDWR);
        if (d >= 0)
            ::shutdown(d, SHUT_RDWR);
    }

    bool home(std::string& path) override
    {
        path = home_;
        return !home_.empty();
    }
    bool list(const std::string& dir, std::vector<Entry>& out) override;
    bool download(const std::string& remote, const std::string& local) override;
    bool upload(const std::string& local, const std::string& remote) override;
    bool makeDir(const std::string& path) override { return simple("MKD " + path, 2); }
    bool removeDir(const std::string& path) override { return simple("RMD " + path, 2); }
    bool removeFile(const std::string& path) override { return simple("DELE " + path, 2); }
    bool rename(const std::string& from, const std::string& to) override
    {
        clearError();
        int code;
        std::string text;
        if (!command("RNFR " + from, code, text))
            return false;
        if (code != 350)
            return fail(text);
        return simple("RNTO " + to, 2);
    }

private:
    void log(LogKind k, const std::string& s) const
    {
        if (hooks_.log)
            hooks_.log(k, s);
    }
    void closeAll()
    {
        int d = data_.exchange(-1), c = ctrl_.exchange(-1);
        closeFd(d);
        closeFd(c);
        buf_.clear();
    }
    bool lostConnection(const std::string& why)
    {
        closeAll();
        return fail(aborted_ ? "cancelled" : why, !aborted_);
    }

    bool readReply(int& code, std::string& text);
    bool command(const std::string& cmd, int& code, std::string& text);
    bool simple(const std::string& cmd, int expectClass);
    bool login();
    // Opens the data connection for `cmd` (RETR, STOR, LIST, MLSD) and
    // sends it; data_ is the connected data socket when true.
    bool startTransfer(const std::string& cmd);
    bool finishTransfer();

    Profile profile_;
    ClientHooks hooks_;
    std::string password_;
    std::atomic<int> ctrl_{-1};
    std::atomic<int> data_{-1};
    std::atomic<bool> aborted_{false};
    std::string buf_;
    std::string home_;
    bool mlsd_ = false;
    bool epsv_ = true;
};

bool FtpClient::readReply(int& code, std::string& text)
{
    for (;;) {
        if (takeFtpReply(buf_, code, text)) {
            log(code >= 400 || code == 0 ? LogKind::Error : LogKind::Reply, text);
            if (code == 421)
                return lostConnection(text);
            return true;
        }
        const int fd = ctrl_;
        if (fd < 0)
            return lostConnection("not connected");
        if (!waitFor(fd, POLLIN, profile_.timeout))
            return lostConnection("the server did not answer in " + std::to_string(profile_.timeout) + " s");
        char chunk[4096];
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return lostConnection(n == 0 ? "the server closed the connection" : errnoText("recv"));
        buf_.append(chunk, static_cast<std::size_t>(n));
    }
}

bool FtpClient::command(const std::string& cmd, int& code, std::string& text)
{
    const int fd = ctrl_;
    if (fd < 0)
        return fail("not connected", true);
    log(LogKind::Command, cmd.compare(0, 5, "PASS ") == 0 ? "PASS ********" : cmd);
    const std::string line = cmd + "\r\n";
    if (!sendAll(fd, line.data(), line.size(), profile_.timeout))
        return lostConnection(errnoText("send"));
    return readReply(code, text);
}

bool FtpClient::simple(const std::string& cmd, int expectClass)
{
    clearError();
    int code;
    std::string text;
    if (!command(cmd, code, text))
        return false;
    return code / 100 == expectClass ? true : fail(text);
}

bool FtpClient::login()
{
    const std::string user = profile_.user.empty() ? "anonymous" : profile_.user;
    int code;
    std::string text;
    if (!command("USER " + user, code, text))
        return false;
    if (code == 331 || code == 332) {
        std::string pass = password_;
        if (pass.empty() && profile_.user.empty())
            pass = "anonymous@";
        if (pass.empty()) {
            if (!hooks_.askSecret || !hooks_.askSecret("Password for " + user + "@" + profile_.host + ":", pass))
                return fail("cancelled");
        }
        if (!command("PASS " + pass, code, text))
            return false;
        if (code / 100 != 2) {
            password_.clear(); // wrong: ask again next time
            return fail(text);
        }
        password_ = pass;
    } else if (code / 100 != 2) {
        return fail(text);
    }
    return true;
}

bool FtpClient::connect()
{
    closeAll();
    clearError();
    aborted_ = false;
    log(LogKind::Info, "Connecting to " + profile_.host + ":" + std::to_string(profile_.effectivePort()) + " (FTP)");
    std::string err;
    const int fd = connectTo(profile_.host, profile_.effectivePort(), profile_.timeout, err);
    if (fd < 0)
        return fail(err);
    ctrl_ = fd;
    int code;
    std::string text;
    do {
        if (!readReply(code, text))
            return false;
    } while (code / 100 == 1); // 120: ready in a few minutes
    if (code != 220) {
        closeAll();
        return fail(text);
    }
    if (!login()) {
        closeAll();
        return false;
    }
    // Features: MLSD for exact listings, UTF8 for names.
    if (command("FEAT", code, text) && code == 211) {
        std::string upper = text;
        for (char& c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        mlsd_ = upper.find("\n MLST") != std::string::npos || upper.find("\nMLST") != std::string::npos;
        if (upper.find("UTF8") != std::string::npos)
            command("OPTS UTF8 ON", code, text);
    }
    if (!connected() || !simple("TYPE I", 2)) {
        closeAll();
        return false;
    }
    if (command("PWD", code, text) && code == 257)
        parsePwd(text, home_);
    if (home_.empty())
        home_ = "/";
    log(LogKind::Info, "Connected");
    return connected();
}

void FtpClient::disconnect()
{
    if (ctrl_ >= 0 && !aborted_) {
        int code;
        std::string text;
        const int saved = profile_.timeout;
        profile_.timeout = 3;
        command("QUIT", code, text);
        profile_.timeout = saved;
    }
    closeAll();
}

bool FtpClient::startTransfer(const std::string& cmd)
{
    int code;
    std::string text;
    sockaddr_storage peer{}, local{};
    socklen_t len = sizeof peer;
    ::getpeername(ctrl_, reinterpret_cast<sockaddr*>(&peer), &len);
    len = sizeof local;
    ::getsockname(ctrl_, reinterpret_cast<sockaddr*>(&local), &len);

    int listener = -1;
    if (!profile_.activeMode) {
        int port = 0;
        if (epsv_) {
            if (!command("EPSV", code, text))
                return false;
            if (code == 229)
                parseEpsvPort(text, port);
            else
                epsv_ = false; // remembered for the session
        }
        if (!port) {
            if (peer.ss_family != AF_INET)
                return fail("the server refused EPSV, and PASV needs IPv4");
            if (!command("PASV", code, text))
                return false;
            if (code != 227 || !parsePasvPort(text, port))
                return fail(text);
        }
        // The control connection's address, not the one in the reply: servers
        // behind NAT announce their private address.
        std::string err;
        const int fd = connectTo(hostOf(peer), port, profile_.timeout, err);
        if (fd < 0)
            return fail(err);
        data_ = fd;
    } else {
        listener = ::socket(local.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_storage bindAddr = local;
        if (bindAddr.ss_family == AF_INET)
            reinterpret_cast<sockaddr_in&>(bindAddr).sin_port = 0;
        else
            reinterpret_cast<sockaddr_in6&>(bindAddr).sin6_port = 0;
        len = bindAddr.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (listener < 0 || ::bind(listener, reinterpret_cast<sockaddr*>(&bindAddr), len) < 0 || ::listen(listener, 1) < 0) {
            const std::string why = errnoText("active mode");
            closeFd(listener);
            return fail(why);
        }
        sockaddr_storage bound{};
        len = sizeof bound;
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &len);
        const int port = portOf(bound);
        const std::string host = hostOf(local);
        std::string portCmd;
        if (local.ss_family == AF_INET) {
            portCmd = "PORT " + host + "," + std::to_string(port / 256) + "," + std::to_string(port % 256);
            for (char& c : portCmd)
                if (c == '.')
                    c = ',';
        } else {
            portCmd = "EPRT |2|" + host + "|" + std::to_string(port) + "|";
        }
        if (!command(portCmd, code, text) || code / 100 != 2) {
            closeFd(listener);
            return error_.empty() ? fail(text) : false;
        }
    }

    if (!command(cmd, code, text) || (code != 125 && code != 150)) {
        int d = data_.exchange(-1);
        closeFd(d);
        closeFd(listener);
        return error_.empty() ? fail(text) : false;
    }
    if (listener >= 0) {
        const bool ready = waitFor(listener, POLLIN, profile_.timeout);
        const int fd = ready ? ::accept(listener, nullptr, nullptr) : -1;
        closeFd(listener);
        if (fd < 0)
            return fail("the server did not open the data connection (active mode)");
        data_ = fd;
    }
    return true;
}

bool FtpClient::finishTransfer()
{
    int d = data_.exchange(-1);
    closeFd(d);
    int code;
    std::string text;
    if (!readReply(code, text))
        return false;
    return code / 100 == 2 ? true : fail(text);
}

bool FtpClient::list(const std::string& dir, std::vector<Entry>& out)
{
    clearError();
    out.clear();
    // CWD first: LIST arguments with spaces are not portable.
    if (!simple("CWD " + dir, 2))
        return false;
    bool started = startTransfer(mlsd_ ? "MLSD" : "LIST -a");
    if (!started && !mlsd_ && !lost_ && connected())
        started = startTransfer("LIST"); // a server that takes no options
    if (!started)
        return false;
    std::string data;
    char chunk[16384];
    for (;;) {
        const int fd = data_;
        if (fd < 0 || !waitFor(fd, POLLIN, profile_.timeout))
            return lostConnection("the listing timed out");
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return lostConnection(errnoText("recv"));
        if (n == 0)
            break;
        data.append(chunk, static_cast<std::size_t>(n));
    }
    if (!finishTransfer())
        return false;
    const std::time_t now = std::time(nullptr);
    std::size_t pos = 0;
    while (pos < data.size()) {
        std::size_t eol = data.find('\n', pos);
        if (eol == std::string::npos)
            eol = data.size();
        const std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        Entry e;
        if (mlsd_ ? parseMlsdLine(line, e) : parseListLine(line, e, now))
            out.push_back(std::move(e));
    }
    return true;
}

bool FtpClient::download(const std::string& remote, const std::string& local)
{
    clearError();
    // Into a temporary file: a failed download keeps the old copy.
    const std::string tmp = local + ".nppremote-part";
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0)
        return fail(errnoText(tmp.c_str()));
    bool ok = startTransfer("RETR " + remote);
    char chunk[65536];
    while (ok) {
        const int fd = data_;
        if (fd < 0 || !waitFor(fd, POLLIN, profile_.timeout)) {
            ok = lostConnection("the download timed out");
            break;
        }
        const ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = lostConnection(errnoText("recv"));
            break;
        }
        if (n == 0)
            break;
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = ::write(out, chunk + done, static_cast<std::size_t>(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0) {
                ok = fail(errnoText(tmp.c_str()));
                break;
            }
            done += w;
        }
    }
    if (::close(out) < 0 && ok)
        ok = fail(errnoText(tmp.c_str()));
    if (ok)
        ok = finishTransfer();
    else if (data_ >= 0) {
        int d = data_.exchange(-1);
        closeFd(d);
        if (connected()) { // the 226/426 that follows the cut transfer
            int code;
            std::string text;
            const std::string why = error_;
            const bool lost = lost_;
            readReply(code, text);
            fail(why, lost || lost_);
        }
    }
    if (ok && ::rename(tmp.c_str(), local.c_str()) < 0)
        ok = fail(errnoText(local.c_str()));
    if (!ok)
        ::unlink(tmp.c_str());
    return ok;
}

bool FtpClient::upload(const std::string& local, const std::string& remote)
{
    clearError();
    const int in = ::open(local.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return fail(errnoText(local.c_str()));
    bool ok = startTransfer("STOR " + remote);
    char chunk[65536];
    while (ok) {
        const ssize_t n = ::read(in, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = fail(errnoText(local.c_str()));
            break;
        }
        if (n == 0)
            break;
        const int fd = data_;
        if (fd < 0 || !sendAll(fd, chunk, static_cast<std::size_t>(n), profile_.timeout)) {
            ok = lostConnection(errnoText("send"));
            break;
        }
    }
    ::close(in);
    if (ok)
        return finishTransfer();
    if (data_ >= 0) {
        int d = data_.exchange(-1);
        closeFd(d);
        if (connected()) {
            int code;
            std::string text;
            const std::string why = error_;
            const bool lost = lost_;
            readReply(code, text);
            fail(why, lost || lost_);
        }
    }
    return false;
}

} // namespace

std::unique_ptr<RemoteClient> makeFtpClient(const Profile& p, const ClientHooks& hooks)
{
    return std::make_unique<FtpClient>(p, hooks);
}

} // namespace nppremote
