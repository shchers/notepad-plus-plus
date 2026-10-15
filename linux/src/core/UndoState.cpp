#include "core/UndoState.h"

#include "platform/FileIO.h"

#include <cinttypes>
#include <cstdio>
#include <sstream>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

constexpr const char* kMagic = "NPPGTK-UNDO 1";

} // namespace

std::uint64_t undoTextHash(std::string_view text)
{
    std::uint64_t h = 14695981039346656037ull; // FNV-1a
    for (const unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool UndoState::fits(std::string_view text) const
{
    return text.size() == textLength && undoTextHash(text) == textHash;
}

fs::path undoStatePath(const fs::path& snapshot)
{
    fs::path p = snapshot;
    p += ".undo";
    return p;
}

// "NPPGTK-UNDO 1\n<length> <hash> <save> <detach> <tentative> <current> <count>\n"
// then per action "<type> <position> <bytes>\n<bytes of text>\n".
bool writeUndoState(const fs::path& file, const UndoState& s, std::string* error)
{
    std::ostringstream os;
    os << kMagic << '\n'
       << s.textLength << ' ' << s.textHash << ' ' << s.savePoint << ' ' << s.detach << ' ' << s.tentative << ' '
       << s.current << ' ' << s.actions.size() << '\n';
    for (const UndoAction& a : s.actions) {
        os << a.type << ' ' << a.position << ' ' << a.text.size() << '\n';
        os.write(a.text.data(), static_cast<std::streamsize>(a.text.size()));
        os << '\n';
    }
    return platform::writeFileAtomic(file, os.str(), error);
}

std::optional<UndoState> readUndoState(const fs::path& file)
{
    std::string d;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || !platform::readFile(file, d, nullptr))
        return std::nullopt;
    std::size_t at = 0;
    auto line = [&](std::string& out) {
        const std::size_t nl = d.find('\n', at);
        if (nl == std::string::npos)
            return false;
        out.assign(d, at, nl - at);
        at = nl + 1;
        return true;
    };
    std::string l;
    if (!line(l) || l != kMagic || !line(l))
        return std::nullopt;
    UndoState s;
    unsigned long long count = 0;
    if (std::sscanf(l.c_str(), "%" SCNu64 " %" SCNu64 " %d %d %d %d %llu", &s.textLength, &s.textHash, &s.savePoint,
                    &s.detach, &s.tentative, &s.current, &count) != 7 ||
        count > d.size())
        return std::nullopt;
    s.actions.reserve(count);
    for (unsigned long long i = 0; i < count; ++i) {
        UndoAction a;
        unsigned long long len = 0;
        if (!line(l) || std::sscanf(l.c_str(), "%d %" SCNd64 " %llu", &a.type, &a.position, &len) != 3 ||
            len > d.size() - at || d.size() - at - len < 1 || d[at + len] != '\n')
            return std::nullopt;
        a.text.assign(d, at, len);
        at += len + 1;
        s.actions.push_back(std::move(a));
    }
    if (s.current < 0 || static_cast<unsigned long long>(s.current) > count)
        return std::nullopt;
    return s;
}

} // namespace npp::core
