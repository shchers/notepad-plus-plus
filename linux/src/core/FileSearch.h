// Find in Files — upstream Notepad_plus::getMatchedFileNames,
// FindReplaceDlg::getAndValidatePatterns and the per-file loop of
// findInFiles / replaceInFiles, without Win32. Files are searched in a
// scratch Scintilla document (upstream's _invisibleEditView) with the same
// SearchEngine as open documents, so results are identical.
#pragma once

#include "core/FileManager.h"
#include "core/SearchEngine.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace npp::core {

// Filter list "*.cpp *.h !*.bak !\build !+\.git" (spaces or ';' separate,
// "double quotes" group). Matching is case-insensitive, as PathMatchSpec.
class FileFilter {
public:
    explicit FileFilter(std::string_view filters);
    bool matchesFile(std::string_view name) const;
    // `level` 1 = directly inside the searched folder.
    bool excludesDir(std::string_view name, int level) const;
    const std::vector<std::string>& patterns() const { return patterns_; }

private:
    std::vector<std::string> patterns_;
};

struct FileCandidate {
    std::filesystem::path path;
    std::uintmax_t size = 0;
};

// Lists matching files below `dir`. `progress` (optional) gets the number found
// so far and returns false to cancel. Hidden (dot) folders are skipped unless
// `hidden`; symlinked folders are not followed.
std::vector<FileCandidate> listFiles(const std::filesystem::path& dir, const FileFilter& filter, bool recursive,
                                     bool hidden, const std::function<bool(std::size_t)>& progress = {});

struct FileHits {
    std::filesystem::path path;
    std::vector<Hit> hits;
};

class FileSearcher {
public:
    // `scratch` must be a Scintilla instance nobody else uses (its document is replaced).
    explicit FileSearcher(Sci scratch) : sci_(scratch) {}

    // Returns false when the file cannot be read or looks binary (skipped).
    bool findInFile(const std::filesystem::path& file, const FindOptions& o, FileHits& out, std::string* error = nullptr);
    // Replace All in one file; writes it back in its own encoding and line
    // endings only when something changed. Returns the count, -1 on error
    // (invalid regex, unreadable, unwritable or unrepresentable text).
    int replaceInFile(const std::filesystem::path& file, const FindOptions& o, std::string* error = nullptr);

private:
    bool load(const std::filesystem::path& file, std::string* error, LoadedFile* lf);
    Sci sci_;
};

} // namespace npp::core
