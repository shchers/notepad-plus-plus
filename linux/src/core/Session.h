// session.xml model — reads and writes the same format as upstream
// NppParameters::getSessionFromXmlTree / writeSession, so the files are
// interchangeable (paths aside). Attributes the port does not use yet
// (document map, tab colour, RTL, pinning) are kept for round-tripping.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace npp::core {

struct SessionFile {
    std::string filename;          // full path, or "new N" for untitled documents
    std::string lang;              // Language menu name ("C++", "Normal text")
    int encoding = -1;             // Windows code page, -1 = Unicode / detect
    bool userReadOnly = false;
    std::string backupFilePath;    // snapshot of unsaved text, empty if none
    std::uint64_t originalFileLastModifTimestamp = 0; // FILETIME of the file when the snapshot was taken

    // Upstream Position.
    std::int64_t firstVisibleLine = 0;
    std::int64_t xOffset = 0;
    std::int64_t scrollWidth = 1;
    std::int64_t startPos = 0;     // anchor
    std::int64_t endPos = 0;       // caret
    std::int64_t selMode = 0;      // SC_SEL_*
    std::int64_t offset = 0;
    std::int64_t wrapCount = 0;

    std::vector<std::int64_t> marks; // bookmarked lines
    std::vector<std::int64_t> folds; // contracted fold header lines

    int tabColourId = -1;
    bool rtl = false;
    bool tabPinned = false;
    bool untitleTabRenamed = false;

    bool operator==(const SessionFile&) const = default;
};

// Folder as Workspace state (upstream <FileBrowser>, written when the panel is open).
struct FileBrowserRoot {
    std::string folder;
    std::vector<std::string> expanded; // expanded sub-folders (full paths)
    bool operator==(const FileBrowserRoot&) const = default;
};

struct Session {
    int activeView = 0;            // 0 = main, 1 = sub
    std::size_t activeIndex[2] = {0, 0};
    std::vector<SessionFile> views[2];
    bool includeFileBrowser = false;
    std::vector<FileBrowserRoot> fileBrowserRoots;
    std::string fileBrowserSelected;

    bool empty() const { return views[0].empty() && views[1].empty(); }
    bool operator==(const Session&) const = default;
};

bool readSession(const std::filesystem::path& file, Session& out, std::string* error = nullptr);
// Writes atomically. Like upstream, keeps the previous file as
// "<file>.inCaseOfCorruption.bak".
bool writeSession(const std::filesystem::path& file, const Session& s, std::string* error = nullptr);

} // namespace npp::core
