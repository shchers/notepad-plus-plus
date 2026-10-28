#include "RemoteUtil.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace nppremote {

namespace {

struct Token {
    std::size_t begin, end;
};

std::vector<Token> tokenize(const std::string& s)
{
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i >= s.size())
            break;
        const std::size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t')
            ++i;
        out.push_back({b, i});
    }
    return out;
}

std::string tok(const std::string& s, const Token& t)
{
    return s.substr(t.begin, t.end - t.begin);
}

bool allDigits(const std::string& s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

std::string chomp(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n'))
        s.pop_back();
    return s;
}

int monthIndex(const std::string& m)
{
    static const char* const names[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (m.size() != 3)
        return -1;
    for (int i = 0; i < 12; ++i)
        if (std::tolower(static_cast<unsigned char>(m[0])) == names[i][0] && std::tolower(static_cast<unsigned char>(m[1])) == names[i][1] &&
            std::tolower(static_cast<unsigned char>(m[2])) == names[i][2])
            return i + 1;
    return -1;
}

int currentYear(std::time_t now)
{
    std::tm tm{};
    gmtime_r(&now, &tm);
    return tm.tm_year + 1900;
}

// "Oct 28 10:15" (within the last months) or "Oct 28 2025".
bool unixDate(const std::string& mon, const std::string& day, const std::string& timeOrYear, std::time_t now, std::time_t& out)
{
    const int m = monthIndex(mon);
    if (m < 0 || !allDigits(day))
        return false;
    const int d = std::atoi(day.c_str());
    if (d < 1 || d > 31)
        return false;
    int hh = 0, mm = 0, year = 0;
    if (timeOrYear.size() == 4 && allDigits(timeOrYear)) {
        year = std::atoi(timeOrYear.c_str());
    } else if (std::sscanf(timeOrYear.c_str(), "%d:%d", &hh, &mm) == 2) {
        year = currentYear(now);
        if (utcTime(year, m, d, hh, mm, 0) > now + 86400) // a date in the future is last year's
            --year;
    } else {
        return false;
    }
    out = utcTime(year, m, d, hh, mm, 0);
    return true;
}

// The rest of `line` after token `t`, one separator skipped.
std::string restAfter(const std::string& line, const Token& t)
{
    return t.end + 1 <= line.size() ? line.substr(t.end + 1) : std::string();
}

bool dotEntry(const std::string& name)
{
    return name == "." || name == "..";
}

// "drwxr-xr-x ... name": the fields up to the name, the name with spaces.
bool parseUnixLong(const std::string& line, Entry& e, std::time_t now)
{
    const std::vector<Token> t = tokenize(line);
    if (t.size() < 6)
        return false;
    const std::string mode = tok(line, t[0]);
    if (mode.size() < 10 || !std::strchr("-dlbcpsD", mode[0]))
        return false;
    // The month names the date; the size is right before it (the owner and
    // group columns vary between servers).
    for (std::size_t i = 2; i + 2 < t.size(); ++i) {
        std::time_t when = 0;
        if (!allDigits(tok(line, t[i - 1])) || !unixDate(tok(line, t[i]), tok(line, t[i + 1]), tok(line, t[i + 2]), now, when))
            continue;
        std::string name = restAfter(line, t[i + 2]);
        if (name.empty())
            return false;
        e = Entry{};
        e.isDir = mode[0] == 'd';
        e.isLink = mode[0] == 'l';
        e.size = std::strtoll(tok(line, t[i - 1]).c_str(), nullptr, 10);
        e.mtime = when;
        if (e.isLink) {
            const std::size_t arrow = name.find(" -> ");
            if (arrow != std::string::npos)
                name.resize(arrow);
        }
        // sftp prints full paths for some listings; a name has no '/'.
        const std::size_t slash = name.rfind('/');
        if (slash != std::string::npos && slash + 1 < name.size())
            name = name.substr(slash + 1);
        e.name = name;
        return !dotEntry(name);
    }
    return false;
}

} // namespace

std::time_t utcTime(int year, int month, int day, int hour, int minute, int second)
{
    // Days from civil (H. Hinnant), valid for the proleptic Gregorian calendar.
    year -= month <= 2;
    const long era = (year >= 0 ? year : year - 399) / 400;
    const long yoe = year - era * 400;
    const long doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + doe - 719468;
    return static_cast<std::time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

bool takeFtpReply(std::string& buf, int& code, std::string& text)
{
    std::size_t eol = buf.find('\n');
    if (eol == std::string::npos)
        return false;
    const std::string first = chomp(buf.substr(0, eol + 1));
    if (first.size() < 3 || !allDigits(first.substr(0, 3))) {
        // Not a reply line: hand it on as code 0 so the caller can fail.
        code = 0;
        text = first;
        buf.erase(0, eol + 1);
        return true;
    }
    std::size_t end = eol + 1;
    if (first.size() > 3 && first[3] == '-') {
        const std::string closing = first.substr(0, 3) + " ";
        for (;;) {
            const std::size_t next = buf.find('\n', end);
            if (next == std::string::npos)
                return false;
            const std::string line = buf.substr(end, next + 1 - end);
            end = next + 1;
            if (line.compare(0, 4, closing) == 0 || chomp(line) == first.substr(0, 3))
                break;
        }
    }
    code = std::atoi(first.substr(0, 3).c_str());
    text = chomp(buf.substr(0, end));
    buf.erase(0, end);
    return true;
}

bool parsePasvPort(const std::string& text, int& port)
{
    // The numbers may come with or without parentheses.
    for (std::size_t i = 4; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            continue;
        int h[4], p1, p2;
        if (std::sscanf(text.c_str() + i, "%d,%d,%d,%d,%d,%d", &h[0], &h[1], &h[2], &h[3], &p1, &p2) == 6) {
            if (p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255)
                return false;
            port = p1 * 256 + p2;
            return port > 0;
        }
    }
    return false;
}

bool parseEpsvPort(const std::string& text, int& port)
{
    const std::size_t open = text.find('(');
    if (open == std::string::npos || open + 4 >= text.size())
        return false;
    const char d = text[open + 1];
    if (text[open + 2] != d || text[open + 3] != d)
        return false;
    const std::size_t close = text.find(d, open + 4);
    if (close == std::string::npos)
        return false;
    const std::string digits = text.substr(open + 4, close - open - 4);
    if (!allDigits(digits))
        return false;
    port = std::atoi(digits.c_str());
    return port > 0 && port < 65536;
}

bool parsePwd(const std::string& text, std::string& path)
{
    const std::size_t open = text.find('"');
    if (open == std::string::npos)
        return false;
    std::string out;
    for (std::size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '"') {
            if (i + 1 < text.size() && text[i + 1] == '"') {
                out += '"';
                ++i;
                continue;
            }
            path = out;
            return !out.empty();
        }
        out += text[i];
    }
    return false;
}

bool parseMlsdLine(const std::string& raw, Entry& e)
{
    const std::string line = chomp(raw);
    const std::size_t sp = line.find(' ');
    if (sp == std::string::npos || sp + 1 >= line.size())
        return false;
    e = Entry{};
    e.name = line.substr(sp + 1);
    std::string type;
    std::size_t i = 0;
    const std::string facts = line.substr(0, sp);
    while (i < facts.size()) {
        std::size_t semi = facts.find(';', i);
        if (semi == std::string::npos)
            semi = facts.size();
        const std::string fact = facts.substr(i, semi - i);
        i = semi + 1;
        const std::size_t eq = fact.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = fact.substr(0, eq);
        for (char& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = fact.substr(eq + 1);
        if (key == "type") {
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            type = value;
        } else if (key == "size" && allDigits(value)) {
            e.size = std::strtoll(value.c_str(), nullptr, 10);
        } else if (key == "modify" && value.size() >= 14) {
            int y, mo, d, h, mi, s;
            if (std::sscanf(value.c_str(), "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6)
                e.mtime = utcTime(y, mo, d, h, mi, s);
        }
    }
    if (type == "cdir" || type == "pdir" || dotEntry(e.name))
        return false;
    e.isDir = type == "dir";
    // OS.unix=slink:/target (vsftpd, ProFTPD) or type=OS.unix=symlink
    e.isLink = type.find("link") != std::string::npos || facts.find("slink") != std::string::npos;
    return !e.name.empty();
}

bool parseListLine(const std::string& raw, Entry& e, std::time_t now)
{
    const std::string line = chomp(raw);
    if (line.empty() || line.compare(0, 6, "total ") == 0)
        return false;
    if (parseUnixLong(line, e, now))
        return true;
    // DOS / IIS: "10-28-26  10:15AM       <DIR>          name"
    const std::vector<Token> t = tokenize(line);
    if (t.size() < 4)
        return false;
    int mo, d, y, h, mi;
    char ampm[3] = {};
    if (std::sscanf(tok(line, t[0]).c_str(), "%d-%d-%d", &mo, &d, &y) != 3)
        return false;
    const int n = std::sscanf(tok(line, t[1]).c_str(), "%d:%d%2s", &h, &mi, ampm);
    if (n < 2)
        return false;
    if (n == 3) {
        const bool pm = std::toupper(static_cast<unsigned char>(ampm[0])) == 'P';
        h = h % 12 + (pm ? 12 : 0);
    }
    if (y < 100)
        y += y < 70 ? 2000 : 1900;
    const std::string what = tok(line, t[2]);
    e = Entry{};
    if (what == "<DIR>")
        e.isDir = true;
    else if (allDigits(what))
        e.size = std::strtoll(what.c_str(), nullptr, 10);
    else
        return false;
    e.mtime = utcTime(y, mo, d, h, mi, 0);
    e.name = restAfter(line, t[2]);
    const std::size_t first = e.name.find_first_not_of(' ');
    e.name = first == std::string::npos ? std::string() : e.name.substr(first);
    return !e.name.empty() && !dotEntry(e.name);
}

bool parseSftpLsLine(const std::string& raw, Entry& e, std::time_t now)
{
    return parseUnixLong(chomp(raw), e, now);
}

bool sftpQuote(const std::string& arg, std::string& out)
{
    out = "\"";
    for (char c : arg) {
        if (c == '\n' || c == '\r')
            return false;
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    out += '"';
    return true;
}

std::string joinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty())
        return "/" + name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string parentPath(const std::string& path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const std::size_t slash = p.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return p.substr(0, slash);
}

std::string resolvePath(const std::string& typed, const std::string& home, const std::string& base)
{
    std::string path;
    if (typed == "~" || typed.compare(0, 2, "~/") == 0)
        path = joinPath(home.empty() ? "/" : home, typed.substr(typed.size() > 1 ? 2 : 1));
    else if (!typed.empty() && typed[0] == '/')
        path = typed;
    else
        path = joinPath(base.empty() ? "/" : base, typed);
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string::npos)
            slash = path.size();
        const std::string part = path.substr(start, slash - start);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = slash + 1;
    }
    std::string out;
    for (const std::string& part : parts)
        out += "/" + part;
    return out.empty() ? "/" : out;
}

std::string baseName(const std::string& path)
{
    std::string p = path;
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool validName(const std::string& name)
{
    return !name.empty() && !dotEntry(name) && name.find('/') == std::string::npos && name.find('\n') == std::string::npos &&
           name.find('\r') == std::string::npos;
}

std::string profileFolder(const std::string& profile)
{
    std::string out;
    for (char c : profile)
        out += c == '/' ? '_' : c;
    while (!out.empty() && out[0] == '.')
        out[0] = '_';
    return out.empty() ? "_" : out;
}

std::string cachePath(const std::string& root, const std::string& profile, const std::string& remote)
{
    std::string r = root;
    while (r.size() > 1 && r.back() == '/')
        r.pop_back();
    std::string rest;
    // ".." would leave the profile's folder: keep each part as a name.
    std::size_t i = 0;
    while (i <= remote.size()) {
        std::size_t slash = remote.find('/', i);
        if (slash == std::string::npos)
            slash = remote.size();
        std::string part = remote.substr(i, slash - i);
        i = slash + 1;
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
            part = "_..";
        rest += "/" + part;
    }
    return r + "/" + profileFolder(profile) + (rest.empty() ? "/" : rest);
}

bool fromCachePath(const std::string& root, const std::string& local, std::string& profileDir, std::string& remote)
{
    std::string r = root;
    while (r.size() > 1 && r.back() == '/')
        r.pop_back();
    if (local.size() <= r.size() + 1 || local.compare(0, r.size(), r) != 0 || local[r.size()] != '/')
        return false;
    const std::string rest = local.substr(r.size() + 1);
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size())
        return false;
    profileDir = rest.substr(0, slash);
    remote = rest.substr(slash);
    return true;
}

std::string formatSize(std::int64_t size)
{
    if (size < 0)
        return {};
    if (size < 1024)
        return std::to_string(size) + " B";
    static const char* const units[] = {"KB", "MB", "GB", "TB"};
    double v = static_cast<double>(size);
    int u = -1;
    do {
        v /= 1024;
        ++u;
    } while (v >= 1024 && u < 3);
    char buf[32];
    std::snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
    return buf;
}

std::string formatTime(std::time_t t)
{
    if (t <= 0)
        return {};
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

} // namespace nppremote
