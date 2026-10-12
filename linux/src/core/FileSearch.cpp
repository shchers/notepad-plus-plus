#include "core/FileSearch.h"

#include "core/FileManager.h"

#include <glib.h>

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

std::string fold(std::string_view s)
{
    gchar* f = g_utf8_casefold(s.data(), static_cast<gssize>(s.size()));
    std::string out = f ? f : std::string(s);
    g_free(f);
    return out;
}

bool globMatch(const std::string& foldedPattern, std::string_view name)
{
    // "*.*" is Windows for "any file", with or without an extension.
    if (foldedPattern == "*.*")
        return true;
    return g_pattern_match_simple(foldedPattern.c_str(), fold(name).c_str());
}

// Looks binary: NUL bytes early on, unless it is UTF-16 with a BOM.
bool looksBinary(const std::string& raw)
{
    if (raw.size() >= 2 && ((raw[0] == '\xFF' && raw[1] == '\xFE') || (raw[0] == '\xFE' && raw[1] == '\xFF')))
        return false;
    return raw.find('\0', 0) < std::min<std::size_t>(raw.size(), 8000);
}

} // namespace

FileFilter::FileFilter(std::string_view filters)
{
    std::string cur;
    bool quoted = false;
    auto flush = [&] {
        if (!cur.empty())
            patterns_.push_back(fold(cur));
        cur.clear();
    };
    for (char c : filters) {
        if (c == '"')
            quoted = !quoted;
        else if (!quoted && (c == ' ' || c == ';' || c == '\t'))
            flush();
        else
            cur += c;
    }
    flush();
    // Upstream getAndValidatePatterns: nothing, or only exclusions, means all files.
    bool inclusion = false;
    for (const std::string& p : patterns_)
        inclusion = inclusion || p[0] != '!';
    if (!inclusion)
        patterns_.insert(patterns_.begin(), "*.*");
}

bool FileFilter::matchesFile(std::string_view name) const
{
    bool matched = false;
    for (const std::string& p : patterns_) {
        if (p.size() > 1 && p[0] == '!') {
            if (p[1] == '\\' || p[1] == '/' || p[1] == '+')
                continue; // folder exclusion
            if (globMatch(p.substr(1), name))
                return false;
            continue;
        }
        matched = matched || globMatch(p, name);
    }
    return matched;
}

bool FileFilter::excludesDir(std::string_view name, int level) const
{
    for (const std::string& p : patterns_) {
        const bool sep2 = p.size() > 2 && (p[2] == '\\' || p[2] == '/');
        if (p.size() > 3 && p[0] == '!' && p[1] == '+' && sep2) {
            if (globMatch(p.substr(3), name))
                return true;
        } else if (p.size() > 2 && p[0] == '!' && (p[1] == '\\' || p[1] == '/')) {
            if (level == 1 && globMatch(p.substr(2), name))
                return true;
        }
    }
    return false;
}

std::vector<FileCandidate> listFiles(const fs::path& dir, const FileFilter& filter, bool recursive, bool hidden,
                                     const std::function<bool(std::size_t)>& progress)
{
    std::vector<FileCandidate> out;
    bool cancelled = false;
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& d, int level) {
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (const auto& e : fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec))
            entries.push_back(e);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
        for (const auto& e : entries) {
            if (cancelled)
                return;
            const std::string name = e.path().filename().string();
            if (e.is_directory(ec) && !e.is_symlink(ec)) {
                if (!recursive || (!hidden && name.starts_with(".")) || filter.excludesDir(name, level))
                    continue;
                walk(e.path(), level + 1);
            } else if (e.is_regular_file(ec) && filter.matchesFile(name)) {
                out.push_back({e.path(), e.file_size(ec)});
                if (progress && !progress(out.size()))
                    cancelled = true;
            }
        }
    };
    walk(dir, 1);
    return out;
}

bool FileSearcher::load(const fs::path& file, std::string* error, LoadedFile* lf)
{
    std::string raw;
    if (!platform::readFile(file, raw, error))
        return false;
    if (looksBinary(raw)) {
        if (error)
            *error = "binary file";
        return false;
    }
    lf->stamp = platform::statFile(file);
    lf->text = platform::decode(raw);
    sci_(SCI_SETREADONLY, 0);
    sci_(SCI_SETUNDOCOLLECTION, 0);
    sci_(SCI_CLEARALL);
    sci_(SCI_SETCODEPAGE, SC_CP_UTF8);
    sci_(SCI_ADDTEXT, lf->text.utf8.size(), reinterpret_cast<sptr_t>(lf->text.utf8.data()));
    sci_(SCI_SETUNDOCOLLECTION, 1);
    sci_(SCI_EMPTYUNDOBUFFER);
    sci_(SCI_SETSEL, 0, 0);
    return true;
}

bool FileSearcher::findInFile(const fs::path& file, const FindOptions& o, FileHits& out, std::string* error)
{
    LoadedFile lf;
    if (!load(file, error, &lf))
        return false;
    out.path = file;
    out.hits.clear();
    FindOptions whole = o;
    whole.inSelection = false;
    if (SearchEngine(sci_).findAll(whole, out.hits, true) < 0) {
        if (error)
            *error = "invalid regular expression";
        return false;
    }
    return true;
}

int FileSearcher::replaceInFile(const fs::path& file, const FindOptions& o, std::string* error)
{
    LoadedFile lf;
    if (!load(file, error, &lf))
        return -1;
    FindOptions whole = o;
    whole.inSelection = false;
    whole.wrap = true; // the whole file
    const int n = SearchEngine(sci_).replaceAll(whole);
    if (n < 0) {
        if (error)
            *error = "invalid regular expression";
        return -1;
    }
    if (n == 0)
        return 0;
    bool unrepresentable = false;
    if (!saveFile(file, sci_.text(), lf.text.encoding, error, &unrepresentable))
        return -1;
    return n;
}

} // namespace npp::core
