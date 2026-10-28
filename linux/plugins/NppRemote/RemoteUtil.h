// NppRemote — text handling without I/O: protocol replies, directory
// listings, quoting, remote paths and the local cache layout. Kept apart so
// the tests check it without a server.
#pragma once

#include "RemoteClient.h"

#include <string>
#include <vector>

namespace nppremote {

// ---- FTP (RFC 959, 2428, 3659) ----

// Takes one complete reply off the front of `buf` (single or multi-line
// "123-...\r\n...123 end\r\n"). False while more data is needed.
bool takeFtpReply(std::string& buf, int& code, std::string& text);
// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)": the port.
bool parsePasvPort(const std::string& text, int& port);
// "229 Entering Extended Passive Mode (|||port|)".
bool parseEpsvPort(const std::string& text, int& port);
// "257 "/dir" is current directory", with "" for a quote in the name.
bool parsePwd(const std::string& text, std::string& path);
// One MLSD line ("type=file;size=12;modify=20261028101500; name"). False
// for the "." and ".." entries (cdir, pdir) and for lines it cannot read.
bool parseMlsdLine(const std::string& line, Entry& e);
// One LIST line: Unix ("-rw-r--r-- 1 u g 12 Oct 28 10:15 name", also
// without the group, "name -> target" for links) or DOS/IIS
// ("10-28-26  10:15AM  <DIR>  name"). `now` decides the year of recent
// Unix dates. False for ".", "..", "total N" and unknown lines.
bool parseListLine(const std::string& line, Entry& e, std::time_t now);

// ---- SFTP (OpenSSH's sftp client) ----

// One line of `ls -lna`: "-rw-r--r--    1 1000  1000  12 Oct 28 10:15 name".
bool parseSftpLsLine(const std::string& line, Entry& e, std::time_t now);
// An argument for an sftp command line: double quotes, with \ and " escaped
// (quoted arguments are not globbed). False for names sftp cannot take
// (line breaks).
bool sftpQuote(const std::string& arg, std::string& out);

// ---- Remote paths ('/' separated, absolute) ----

std::string joinPath(const std::string& dir, const std::string& name);
std::string parentPath(const std::string& path); // "/a/b" -> "/a", "/a" -> "/"
// A folder the user typed, made absolute: "~" and "~/x" start at `home`
// (the login folder), other relative paths at `base`. "." and ".." and
// repeated or trailing slashes are folded: "~/a/../b/" -> "<home>/b".
std::string resolvePath(const std::string& typed, const std::string& home, const std::string& base);
std::string baseName(const std::string& path);   // "/a/b" -> "b"
// A file or folder name the user typed: not empty, no '/', not "." or "..".
bool validName(const std::string& name);

// ---- Local cache: <root>/<profile>/<remote path> ----

// A profile name as a folder name ('/' and leading dots replaced).
std::string profileFolder(const std::string& profile);
std::string cachePath(const std::string& root, const std::string& profile, const std::string& remote);
// The profile folder and the remote path of a file in the cache.
bool fromCachePath(const std::string& root, const std::string& local, std::string& profileDir, std::string& remote);

// ---- Display ----

std::string formatSize(std::int64_t size); // "", "12 B", "3.4 KB", ...
std::string formatTime(std::time_t t);     // "2026-10-28 10:15", "" when unknown

// UTC broken-down time to time_t (no timegm in POSIX).
std::time_t utcTime(int year, int month, int day, int hour, int minute, int second);

} // namespace nppremote
