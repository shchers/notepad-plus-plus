// Persistent user settings — the port's stand-in for upstream config.xml
// (NppGUI). Stored as an INI file via GKeyFile at
// $XDG_CONFIG_HOME/notepad-plus-plus-gtk/settings.ini. Settings from a
// Windows config.xml can be imported (core/NppConfig).
#pragma once

#include <filesystem>
#include <array>
#include <map>
#include <string>
#include <vector>

namespace npp::platform {

// Upstream BackupFeature (Preferences > Backup > "Backup on save").
enum class SaveBackup { None = 0, Simple = 1, Verbose = 2 };
// Upstream "File Status Auto-Detection" (Preferences > MISC.).
enum class ChangeDetection { Off = 0, Notify = 1, AutoUpdate = 2 };

// Find/Replace dialog state (upstream config.xml <FindHistory>).
struct FindSettings {
    std::vector<std::string> what, with, filters, directories; // most recent first
    int mode = 0;                 // core::SearchMode
    bool matchCase = false;
    bool wholeWord = false;
    bool wrap = true;
    bool backward = false;
    bool dotMatchesNewline = false;
    bool inSelection = false;
    bool recursive = true;
    bool hidden = false;
    bool bookmarkLine = false;
    bool purge = false;
    static constexpr std::size_t kMaxHistory = 10; // upstream nbMaxFindHistoryPath
};

struct Settings {
    std::string theme;          // theme file name in themes/, empty = stylers.model.xml
    std::string localization;   // nativeLang file name, empty = english.xml
    bool wordWrap = false;
    bool showSpaces = false;    // View > Show Symbol (upstream whiteSpaceShow)
    bool showEol = false;       // (eolShow); both on = "Show All Characters"
    bool showIndentGuides = true;
    bool showWrapSymbol = false;
    int zoom = 0;
    // Preferences > General / Editing / Margins (upstream NppGUI, ScintillaViewParams)
    bool hideToolbar = false;
    bool hideStatusBar = false;
    int wrapIndent = 0;            // 0 Default (fixed), 1 Aligned, 2 Indent
    int caretWidth = 1;            // pixels, 0 = block
    int caretBlinkMs = 600;        // 0 = no blinking
    bool scrollBeyondLastLine = true;
    bool virtualSpace = false;
    bool currentLineHighlight = true;
    int foldStyle = 3;             // 0 Simple, 1 Arrow, 2 Circle tree, 3 Box tree, 4 None
    bool showLineNumbers = true;
    bool showBookmarkMargin = true;
    int edgeMode = 0;              // 0 none, 1 line, 2 background
    int edgeColumn = 80;
    // Preferences > New Document
    int newDocEol = 2;             // SC_EOL_LF
    int newDocEncoding = 0;        // 0 UTF-8, 1 UTF-8 BOM, 2 UTF-16 BE BOM, 3 UTF-16 LE BOM
    std::string newDocLanguage;    // langs.model.xml name, empty = Normal text
    int tabSize = 4;
    bool replaceTabsBySpace = false;
    bool autoIndent = true;

    // Session and backup, upstream defaults.
    bool rememberSession = true;
    bool snapshot = true;          // session snapshot + periodic backup of unsaved documents
    int snapshotIntervalMs = 7000;
    SaveBackup saveBackup = SaveBackup::None;
    std::string saveBackupDir;     // empty = next to the file (upstream "useCustumDir" off)
    ChangeDetection changeDetection = ChangeDetection::Notify;

    FindSettings find;

    int maxRecentFiles = 10;
    std::vector<std::string> recentFiles; // most recent first
    std::vector<std::string> runHistory;  // Run... dialog, most recent first

    // Docking panels: DockManager::saveState() ("id" -> "area,shown,...").
    std::map<std::string, std::string> docking;
    std::array<std::string, 3> projectWorkspaces; // Project Panel 1-3 workspace files
    bool functionListSorted = false;

    int width = 1100;
    int height = 750;
    bool maximized = false;
    int splitPosition = -1;        // divider between the two views, -1 = half

    static std::filesystem::path defaultPath();
    // Returns false when the file does not exist yet (first run); defaults stay.
    bool load(const std::filesystem::path& file = defaultPath());
    bool save(const std::filesystem::path& file = defaultPath()) const;
};

} // namespace npp::platform
