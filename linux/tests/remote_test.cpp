// Bundled plugin Remote Files (linux/plugins/NppRemote): the listing and
// reply parsers, the FTP client against a small FTP server run here (POSIX
// sockets, passive and active mode, MLSD and LIST), the Session worker with
// its reconnect, and the SFTP client against a private sshd started for the
// test when OpenSSH is installed (a passphrase-protected key and an unknown
// host key, so both of ssh's terminal questions are answered).

#include "RemoteClient.h"
#include "RemoteUtil.h"
#include "Session.h"

#include "FakeFtpServer.h"

#include <glib.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace nppremote;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

namespace {

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

bool hasEntry(const std::vector<Entry>& v, const std::string& name, bool dir)
{
    for (const Entry& e : v)
        if (e.name == name && e.isDir == dir)
            return true;
    return false;
}

// ============================================================================
// Parsers
// ============================================================================

void testParsers()
{
    std::string buf = "220-Welcome\r\n to the test\r\n220 Ready\r\n331 Pass";
    int code = 0;
    std::string text;
    CHECK(takeFtpReply(buf, code, text));
    CHECK(code == 220);
    CHECK(text == "220-Welcome\r\n to the test\r\n220 Ready");
    CHECK(buf == "331 Pass");
    CHECK(!takeFtpReply(buf, code, text)); // incomplete
    buf += "word required\r\n";
    CHECK(takeFtpReply(buf, code, text) && code == 331 && buf.empty());

    int port = 0;
    CHECK(parsePasvPort("227 Entering Passive Mode (192,168,1,2,195,149).", port) && port == 195 * 256 + 149);
    CHECK(parsePasvPort("227 =10,0,0,1,4,1", port) && port == 1025);
    CHECK(parseEpsvPort("229 Entering Extended Passive Mode (|||50123|)", port) && port == 50123);
    CHECK(!parseEpsvPort("229 Extended Passive Mode (|||abc|)", port));
    std::string path;
    CHECK(parsePwd("257 \"/home/a \"\"b\"\"\" is current directory", path) && path == "/home/a \"b\"");

    Entry e;
    CHECK(parseMlsdLine("type=file;size=1234;modify=20261028101530;perm=r; notes v2.txt\r", e));
    CHECK(e.name == "notes v2.txt" && !e.isDir && e.size == 1234 && e.mtime == utcTime(2026, 10, 28, 10, 15, 30));
    CHECK(parseMlsdLine("Type=dir;Modify=20260101000000; src", e) && e.isDir && e.name == "src");
    CHECK(!parseMlsdLine("type=cdir; /home/x", e));
    CHECK(!parseMlsdLine("type=pdir; ..", e));
    CHECK(parseMlsdLine("type=OS.unix=symlink;size=4; link", e) && e.isLink);

    const std::time_t now = utcTime(2026, 10, 28, 12, 0, 0);
    CHECK(parseListLine("-rw-r--r--   1 owner  group     4096 Oct 28 10:15 a file.txt", e, now));
    CHECK(e.name == "a file.txt" && e.size == 4096 && !e.isDir && e.mtime == utcTime(2026, 10, 28, 10, 15, 0));
    CHECK(parseListLine("drwxr-xr-x 2 owner group 4096 Dec 31  2024 old", e, now) && e.isDir && e.mtime == utcTime(2024, 12, 31, 0, 0, 0));
    // A recent-format date later than today is last year's.
    CHECK(parseListLine("-rw-r--r-- 1 o g 1 Nov 30 09:00 x", e, now) && e.mtime == utcTime(2025, 11, 30, 9, 0, 0));
    CHECK(parseListLine("lrwxrwxrwx 1 o g 7 Oct 1 2025 www -> /var/www", e, now) && e.isLink && e.name == "www");
    CHECK(parseListLine("-rw-r--r-- 1 ftp 12 Oct 28 10:15 nogroup", e, now) && e.name == "nogroup" && e.size == 12);
    CHECK(parseListLine("10-28-26  10:15AM       <DIR>          Projects", e, now) && e.isDir && e.name == "Projects");
    CHECK(parseListLine("10-28-26  01:05PM              1234 read me.txt", e, now) && e.size == 1234 && e.name == "read me.txt" &&
          e.mtime == utcTime(2026, 10, 28, 13, 5, 0));
    CHECK(!parseListLine("total 12", e, now));
    CHECK(!parseListLine("drwxr-xr-x 2 o g 4096 Oct 28 10:15 ..", e, now));

    // OpenSSH sftp's "ls -lan" (link count "?" from servers without it).
    CHECK(parseSftpLsLine("-rw-rw-r--    ? 1000     1000            3 Oct 28 12:57 a b.txt", e, now) && e.name == "a b.txt" && e.size == 3);
    CHECK(parseSftpLsLine("drwxr-xr-x    2 1000     1000         4096 Oct  8 09:01 sub dir", e, now) && e.isDir && e.name == "sub dir");
    CHECK(!parseSftpLsLine("drwxr-xr-x    2 1000     1000         4096 Oct  8 09:01 .", e, now));

    std::string q;
    CHECK(sftpQuote("a \"b\"\\c*", q) && q == "\"a \\\"b\\\"\\\\c*\"");
    CHECK(!sftpQuote("two\nlines", q));

    CHECK(joinPath("/", "a") == "/a" && joinPath("/x", "a") == "/x/a" && joinPath("/x/", "a") == "/x/a");
    CHECK(parentPath("/a/b") == "/a" && parentPath("/a") == "/" && parentPath("/") == "/" && parentPath("/a/b/") == "/a");
    CHECK(resolvePath("~/", "/home/u", "/x") == "/home/u" && resolvePath("~", "/home/u", "/x") == "/home/u");
    CHECK(resolvePath("~/www/./a//", "/home/u", "/x") == "/home/u/www/a" && resolvePath("~/../..", "/home/u", "/x") == "/");
    CHECK(resolvePath("sub", "/home/u", "/x") == "/x/sub" && resolvePath("../y", "/home/u", "/x") == "/y");
    CHECK(resolvePath("/etc/", "/home/u", "/x") == "/etc" && resolvePath("~user", "/home/u", "/x") == "/x/~user");
    CHECK(baseName("/a/b") == "b");
    CHECK(validName("x.txt") && !validName("a/b") && !validName("..") && !validName(""));

    CHECK(cachePath("/c", "my/site", "/var/www/i.html") == "/c/my_site/var/www/i.html");
    CHECK(cachePath("/c", ".hidden", "/a/../b") == "/c/_hidden/a/_../b");
    std::string folder, remote;
    CHECK(fromCachePath("/c", "/c/my_site/var/www/i.html", folder, remote) && folder == "my_site" && remote == "/var/www/i.html");
    CHECK(!fromCachePath("/c", "/cc/x/y", folder, remote));
    CHECK(!fromCachePath("/c", "/c/onlyprofile", folder, remote));

    CHECK(formatSize(-1).empty() && formatSize(12) == "12 B" && formatSize(1536) == "1.5 KB" && formatSize(50 * 1048576LL) == "50 MB");
    CHECK(formatTime(utcTime(2026, 10, 28, 10, 15, 0)) == "2026-10-28 10:15");
}

// The calls every protocol must get right, on the server's folder `home`.
void exerciseClient(RemoteClient& c, const fs::path& work, const std::string& home, const fs::path& serverHome, const char* what)
{
    std::fprintf(stderr, "  %s\n", what);
    std::vector<Entry> list;
    CHECK(c.list(home, list));
    CHECK(hasEntry(list, "hello.txt", false));
    CHECK(hasEntry(list, "sub dir", true));
    for (const Entry& e : list)
        if (e.name == "hello.txt")
            CHECK(e.size == 13);

    const fs::path local = work / "dl.txt";
    CHECK(c.download(joinPath(home, "hello.txt"), local.string()));
    CHECK(readAll(local) == "Hello, world\n");
    CHECK(!fs::exists(work / "dl.txt.nppremote-part"));
    // A missing file fails, and the session goes on.
    CHECK(!c.download(joinPath(home, "missing.txt"), (work / "missing.txt").string()));
    CHECK(!c.error().empty());
    CHECK(!fs::exists(work / "missing.txt"));
    CHECK(c.connected());

    std::string binary;
    for (int i = 0; i < 300000; ++i)
        binary += static_cast<char>(i * 7 % 256);
    writeAll(work / "up.bin", binary);
    const std::string upName = "up \"quoted\" name.bin";
    CHECK(c.upload((work / "up.bin").string(), joinPath(home, upName)));
    CHECK(readAll(serverHome / upName) == binary);

    CHECK(c.makeDir(joinPath(home, "new folder")));
    CHECK(fs::is_directory(serverHome / "new folder"));
    CHECK(c.upload((work / "up.bin").string(), joinPath(joinPath(home, "new folder"), "x.bin")));
    CHECK(c.list(joinPath(home, "new folder"), list) && hasEntry(list, "x.bin", false));
    CHECK(!c.removeDir(joinPath(home, "new folder"))); // not empty
    CHECK(c.removeFile(joinPath(joinPath(home, "new folder"), "x.bin")));
    CHECK(c.removeDir(joinPath(home, "new folder")));
    CHECK(!fs::exists(serverHome / "new folder"));

    CHECK(c.rename(joinPath(home, upName), joinPath(home, "renamed.bin")));
    CHECK(fs::exists(serverHome / "renamed.bin") && !fs::exists(serverHome / upName));
    CHECK(c.removeFile(joinPath(home, "renamed.bin")));
    CHECK(!fs::exists(serverHome / "renamed.bin"));
}

void seed(const fs::path& home)
{
    fs::create_directories(home / "sub dir");
    writeAll(home / "hello.txt", "Hello, world\n");
}

void testFtp(const fs::path& tmp)
{
    const fs::path root = tmp / "ftp-root";
    seed(root);
    FakeFtpServer server(root);
    int asked = 0;
    ClientHooks hooks;
    hooks.askSecret = [&](const std::string& prompt, std::string& answer) {
        ++asked;
        CHECK(prompt.find("alice@127.0.0.1") != std::string::npos);
        answer = "secret";
        return true;
    };

    Profile p;
    p.protocol = Protocol::Ftp;
    p.host = "127.0.0.1";
    p.port = server.port();
    p.user = "alice";
    p.timeout = 5;
    {
        auto c = makeClient(p, hooks);
        CHECK(c->connect());
        CHECK(asked == 1);
        std::string home;
        CHECK(c->home(home) && home == "/");
        exerciseClient(*c, tmp, "/", root, "FTP: passive (EPSV), MLSD");
        c->disconnect();
        CHECK(!c->connected());
    }
    {
        // PASV with a NAT address in the reply, LIST, password from the profile.
        server.mlsd = false;
        server.epsv = false;
        Profile q = p;
        q.password = "secret";
        auto c = makeClient(q, hooks);
        CHECK(c->connect());
        CHECK(asked == 1);
        exerciseClient(*c, tmp, "/", root, "FTP: passive (PASV), LIST");
    }
    {
        Profile q = p;
        q.password = "secret";
        q.activeMode = true;
        auto c = makeClient(q, hooks);
        CHECK(c->connect());
        exerciseClient(*c, tmp, "/", root, "FTP: active (PORT)");
    }
    {
        Profile q = p;
        q.password = "wrong";
        auto c = makeClient(q, hooks);
        CHECK(!c->connect());
        CHECK(c->error().find("530") != std::string::npos);
        CHECK(!c->connected());
    }
    {
        Profile q = p;
        q.port = 1; // nothing listens there
        auto c = makeClient(q, hooks);
        CHECK(!c->connect());
        CHECK(!c->error().empty());
    }
}

// The worker: results on the GTK (here: default) main context, and one
// reconnect when the server dropped the connection.
void testSession(const fs::path& tmp)
{
    const fs::path root = tmp / "ftp-session";
    seed(root);
    FakeFtpServer server(root);
    Profile p;
    p.protocol = Protocol::Ftp;
    p.host = "127.0.0.1";
    p.port = server.port();
    p.user = "alice";
    p.password = "secret";
    p.timeout = 5;
    std::vector<bool> states;
    Session::Hooks hooks;
    hooks.connectionChanged = [&](bool c) { states.push_back(c); };
    Session s(p, hooks);
    auto waitFor = [](const bool& flag) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!flag && std::chrono::steady_clock::now() < end)
            g_main_context_iteration(nullptr, FALSE), std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    bool done = false, ok = false;
    std::vector<Entry> list;
    s.post([&](RemoteClient& c) { return c.list("/", list); }, [&](bool r, const std::string&) {
        ok = r;
        done = true;
    });
    waitFor(done);
    CHECK(done && ok && hasEntry(list, "hello.txt", false));
    CHECK(server.logins == 1);
    CHECK(!states.empty() && states.back());

    // The server dropped the line meanwhile: the job fails on it, then the
    // worker logs in again and runs it once more.
    server.dropNext = true;
    done = ok = false;
    s.post([&](RemoteClient& c) { return c.list("/sub dir", list); }, [&](bool r, const std::string&) {
        ok = r;
        done = true;
    });
    waitFor(done);
    CHECK(done && ok);
    CHECK(server.logins == 2);
    CHECK(!states.empty() && states.back()); // back up within the job: no "offline" in between
}

// ============================================================================
// SFTP with a private sshd
// ============================================================================

bool run(const std::string& cmd)
{
    return std::system(cmd.c_str()) == 0;
}

int freePort()
{
    int port = 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

void testSftp(const fs::path& tmp)
{
    const char* sshdPaths[] = {"/usr/sbin/sshd", "/usr/bin/sshd", "/usr/local/sbin/sshd"};
    std::string sshd;
    for (const char* s : sshdPaths)
        if (::access(s, X_OK) == 0)
            sshd = s;
    if (sshd.empty() || !run("command -v sftp >/dev/null && command -v ssh-keygen >/dev/null")) {
        std::printf("  SFTP skipped: OpenSSH (sshd, sftp, ssh-keygen) is not installed\n");
        return;
    }
    const fs::path dir = tmp / "sshd";
    fs::create_directories(dir);
    const std::string d = dir.string();
    if (!run("ssh-keygen -q -t ed25519 -N '' -f '" + d + "/host' && ssh-keygen -q -t ed25519 -N 'pass phrase' -f '" + d + "/client'")) {
        std::printf("  SFTP skipped: ssh-keygen failed\n");
        return;
    }
    fs::copy_file(dir / "client.pub", dir / "authorized_keys");
    const int port = freePort();
    writeAll(dir / "sshd_config", "Port " + std::to_string(port) + "\nListenAddress 127.0.0.1\nHostKey " + d + "/host\nAuthorizedKeysFile " + d +
                                      "/authorized_keys\nPidFile " + d + "/sshd.pid\nUsePAM no\nStrictModes no\nPasswordAuthentication no\n"
                                      "KbdInteractiveAuthentication no\nSubsystem sftp internal-sftp\n");
    // -D: in the foreground, so the test owns it; -e: log to stderr.
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int null = ::open("/dev/null", O_WRONLY);
        ::dup2(null, 2);
        ::execl(sshd.c_str(), sshd.c_str(), "-D", "-e", "-f", (d + "/sshd_config").c_str(), nullptr);
        ::_exit(127);
    }
    bool up = false;
    for (int i = 0; i < 100 && !up; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        up = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        ::close(fd);
        if (!up)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!up) {
        std::printf("  SFTP skipped: the test sshd did not start\n");
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        return;
    }

    const fs::path home = tmp / "sftp-home";
    seed(home);
    Profile p;
    p.protocol = Protocol::Sftp;
    p.host = "127.0.0.1";
    p.port = port;
    p.identityFile = d + "/client";
    p.timeout = 10;
    // Not the user's ssh configuration or known hosts.
    p.sftpOptions = "-F /dev/null -o 'UserKnownHostsFile=" + d + "/known_hosts' -o IdentitiesOnly=yes -o IdentityAgent=none";
    int confirms = 0, secrets = 0;
    ClientHooks hooks;
    if (std::getenv("NPPREMOTE_TEST_LOG"))
        hooks.log = [](LogKind, const std::string& s) { std::fprintf(stderr, "    | %s\n", s.c_str()); };
    hooks.confirm = [&](const std::string& q) {
        ++confirms;
        CHECK(q.find("authenticity") != std::string::npos || q.find("fingerprint") != std::string::npos);
        return true;
    };
    hooks.askSecret = [&](const std::string& prompt, std::string& answer) {
        ++secrets;
        CHECK(prompt.find("passphrase") != std::string::npos);
        answer = "pass phrase";
        return true;
    };
    {
        auto c = makeClient(p, hooks);
        CHECK(c->connect());
        if (!c->connected())
            std::fprintf(stderr, "  sftp: %s\n", c->error().c_str());
        CHECK(confirms == 1);
        CHECK(secrets == 1);
        std::string h;
        CHECK(c->home(h) && !h.empty());
        exerciseClient(*c, tmp, home.string(), home, "SFTP: OpenSSH sftp in a pseudo-terminal");
        c->disconnect();
        CHECK(!c->connected());
    }
    {
        // Known host now; a cancelled passphrase stops the connection.
        auto cancel = hooks;
        cancel.askSecret = [&](const std::string&, std::string&) { return false; };
        auto c = makeClient(p, cancel);
        CHECK(!c->connect());
        CHECK(c->error() == "cancelled");
        CHECK(confirms == 1);
    }
    {
        // A rejected host key ends it as well.
        fs::remove(dir / "known_hosts");
        auto reject = hooks;
        reject.confirm = [](const std::string&) { return false; };
        auto c = makeClient(p, reject);
        CHECK(!c->connect());
        CHECK(!c->connected());
        CHECK(c->error().find("erification") != std::string::npos || c->error().find("ost key") != std::string::npos);
    }
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
}

} // namespace

int main()
{
    // SIGPIPE stays fatal, as in Notepad++: the clients must never raise it.
    char tmpl[] = "/tmp/nppremote-test-XXXXXX";
    const fs::path tmp = ::mkdtemp(tmpl);

    testParsers();
    testFtp(tmp);
    testSession(tmp);
    testSftp(tmp);

    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
