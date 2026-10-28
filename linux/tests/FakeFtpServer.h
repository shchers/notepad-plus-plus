// A small FTP server for the Remote Files tests: one client at a time, over
// a local folder, on 127.0.0.1. Passive (EPSV, or PASV with an address the
// client must ignore) and active (PORT) data connections; MLSD or LIST.
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace fake_ftp {

namespace fs = std::filesystem;
inline std::string joinPath(const std::string& dir, const std::string& name)
{
    return dir.empty() || dir.back() != '/' ? dir + "/" + name : dir + name;
}

inline std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void writeAll(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

class FakeFtpServer {
public:
    std::atomic<bool> mlsd{true};
    std::atomic<bool> epsv{true};
    std::atomic<bool> dropNext{false}; // close the connection at the next command, as an idle timeout
    std::atomic<int> logins{0};

    explicit FakeFtpServer(fs::path root) : root_(std::move(root))
    {
        listen_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(listen_, 4);
        socklen_t len = sizeof a;
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeFtpServer()
    {
        stop_ = true;
        ::shutdown(listen_, SHUT_RDWR);
        ::close(listen_);
        thread_.join();
    }
    int port() const { return port_; }

private:
    fs::path real(const std::string& cwd, const std::string& arg) const
    {
        const std::string p = arg.empty() ? cwd : arg[0] == '/' ? arg : joinPath(cwd, arg);
        return root_ / fs::path(p).relative_path();
    }

    static void sendLine(int fd, const std::string& s)
    {
        const std::string l = s + "\r\n";
        ::send(fd, l.data(), l.size(), MSG_NOSIGNAL);
    }

    static int listenOn(int& port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        ::listen(fd, 1);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        return fd;
    }

    std::string listing(const fs::path& dir) const
    {
        std::string out;
        for (const auto& e : fs::directory_iterator(dir)) {
            const bool d = e.is_directory();
            const std::string name = e.path().filename().string();
            const std::uintmax_t size = d ? 4096 : e.file_size();
            if (mlsd)
                out += std::string("type=") + (d ? "dir" : "file") + ";size=" + std::to_string(size) + ";modify=20261027080000; " + name + "\r\n";
            else
                out += std::string(d ? "drwxr-xr-x" : "-rw-r--r--") + "   1 user     group " + std::to_string(size) + " Oct 27 08:00 " + name + "\r\n";
        }
        if (mlsd)
            out = "type=cdir;modify=20261027080000; .\r\n" + out;
        return out;
    }

    void session(int c)
    {
        std::string cwd = "/", rnfr, buf;
        bool user = false;
        int passive = -1;
        sockaddr_in active{};
        bool haveActive = false;
        sendLine(c, "220 Fake FTP ready");
        auto openData = [&]() -> int {
            if (passive >= 0) {
                pollfd p{passive, POLLIN, 0};
                const int d = ::poll(&p, 1, 5000) > 0 ? ::accept(passive, nullptr, nullptr) : -1;
                ::close(passive);
                passive = -1;
                return d;
            }
            if (haveActive) {
                const int d = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (::connect(d, reinterpret_cast<sockaddr*>(&active), sizeof active) == 0)
                    return d;
                ::close(d);
            }
            return -1;
        };
        for (;;) {
            std::size_t eol;
            while ((eol = buf.find("\r\n")) == std::string::npos) {
                char chunk[1024];
                const ssize_t n = ::recv(c, chunk, sizeof chunk, 0);
                if (n <= 0)
                    return;
                buf.append(chunk, static_cast<std::size_t>(n));
            }
            const std::string line = buf.substr(0, eol);
            buf.erase(0, eol + 2);
            if (dropNext.exchange(false))
                return;
            const std::size_t sp = line.find(' ');
            const std::string cmd = line.substr(0, sp);
            const std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
            std::error_code ec;
            if (cmd == "USER") {
                user = true;
                sendLine(c, "331 Password required");
            } else if (cmd == "PASS") {
                if (user && arg == "secret") {
                    ++logins;
                    sendLine(c, "230 Logged in");
                } else {
                    sendLine(c, "530 Login incorrect");
                }
            } else if (cmd == "FEAT") {
                std::string f = "211-Features:\r\n";
                if (mlsd)
                    f += " MLST type*;size*;modify*;\r\n";
                if (epsv)
                    f += " EPSV\r\n";
                f += " UTF8\r\n211 End";
                sendLine(c, f);
            } else if (cmd == "OPTS" || cmd == "TYPE" || cmd == "NOOP") {
                sendLine(c, "200 OK");
            } else if (cmd == "PWD") {
                sendLine(c, "257 \"" + cwd + "\" is the current directory");
            } else if (cmd == "CWD") {
                const std::string target = arg[0] == '/' ? arg : joinPath(cwd, arg);
                if (fs::is_directory(real(cwd, arg), ec)) {
                    cwd = target;
                    sendLine(c, "250 OK");
                } else {
                    sendLine(c, "550 No such directory");
                }
            } else if (cmd == "EPSV" && epsv) {
                int p;
                passive = listenOn(p);
                sendLine(c, "229 Entering Extended Passive Mode (|||" + std::to_string(p) + "|)");
            } else if (cmd == "PASV") {
                int p;
                passive = listenOn(p);
                // An address the client must ignore (as behind NAT).
                sendLine(c, "227 Entering Passive Mode (10,9,8,7," + std::to_string(p / 256) + "," + std::to_string(p % 256) + ")");
            } else if (cmd == "PORT") {
                int h[4], p1, p2;
                if (std::sscanf(arg.c_str(), "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &p1, &p2) == 6) {
                    active.sin_family = AF_INET;
                    active.sin_port = htons(static_cast<uint16_t>(p1 * 256 + p2));
                    active.sin_addr.s_addr = htonl(static_cast<uint32_t>(h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3]));
                    haveActive = true;
                    sendLine(c, "200 PORT OK");
                } else {
                    sendLine(c, "501 Bad PORT");
                }
            } else if (cmd == "MLSD" || cmd == "LIST") {
                if (cmd == "MLSD" && !mlsd) {
                    sendLine(c, "500 Unknown command");
                    continue;
                }
                const std::string dir = arg.empty() || arg[0] == '-' ? "" : arg;
                sendLine(c, "150 Here comes the listing");
                const int d = openData();
                const std::string data = listing(real(cwd, dir));
                ::send(d, data.data(), data.size(), MSG_NOSIGNAL);
                ::close(d);
                sendLine(c, "226 Done");
            } else if (cmd == "RETR") {
                const fs::path f = real(cwd, arg);
                if (!fs::is_regular_file(f, ec)) {
                    // Data connection opened by the client is dropped.
                    if (passive >= 0)
                        ::close(passive), passive = -1;
                    sendLine(c, "550 No such file");
                    continue;
                }
                sendLine(c, "150 Opening data connection");
                const int d = openData();
                const std::string data = readAll(f);
                ::send(d, data.data(), data.size(), MSG_NOSIGNAL);
                ::close(d);
                sendLine(c, "226 Transfer complete");
            } else if (cmd == "STOR") {
                sendLine(c, "150 Ok to send data");
                const int d = openData();
                std::string data;
                char chunk[4096];
                ssize_t n;
                while ((n = ::recv(d, chunk, sizeof chunk, 0)) > 0)
                    data.append(chunk, static_cast<std::size_t>(n));
                ::close(d);
                writeAll(real(cwd, arg), data);
                sendLine(c, "226 Transfer complete");
            } else if (cmd == "MKD") {
                sendLine(c, fs::create_directory(real(cwd, arg), ec) ? "257 \"" + arg + "\" created" : "550 Cannot create");
            } else if (cmd == "RMD") {
                sendLine(c, fs::is_directory(real(cwd, arg), ec) && fs::is_empty(real(cwd, arg), ec) && fs::remove(real(cwd, arg), ec)
                                ? "250 Removed" : "550 Cannot remove");
            } else if (cmd == "DELE") {
                sendLine(c, fs::is_regular_file(real(cwd, arg), ec) && fs::remove(real(cwd, arg), ec) ? "250 Deleted" : "550 Cannot delete");
            } else if (cmd == "RNFR") {
                rnfr = arg;
                sendLine(c, fs::exists(real(cwd, arg), ec) ? "350 Ready" : "550 No such file");
            } else if (cmd == "RNTO") {
                fs::rename(real(cwd, rnfr), real(cwd, arg), ec);
                sendLine(c, ec ? "550 Cannot rename" : "250 Renamed");
            } else if (cmd == "QUIT") {
                sendLine(c, "221 Bye");
                return;
            } else {
                sendLine(c, "500 Unknown command");
            }
        }
    }

    void serve()
    {
        while (!stop_) {
            const int c = ::accept(listen_, nullptr, nullptr);
            if (c < 0)
                continue;
            session(c);
            ::close(c);
        }
    }

    fs::path root_;
    int listen_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace fake_ftp

using fake_ftp::FakeFtpServer;
