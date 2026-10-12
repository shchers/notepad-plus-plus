// Persistent user settings — the port's stand-in for upstream config.xml
// (NppGUI). Stored as an INI file via GKeyFile at
// $XDG_CONFIG_HOME/notepad-plus-plus-gtk/settings.ini. Settings from a
// Windows config.xml can be imported (core/NppConfig).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace npp::platform {

// Upstream BackupFeature (Preferences > Backup > "Backup on save").
enum class SaveBackup { None = 0, Simple = 1, Verbose = 2 };
// Upstream "File Status Auto-Detection" (Preferences > MISC.).
enum class ChangeDetection { Off = 0, Notify = 1, AutoUpdate = 2 };

struct Settings {
    std::string theme;          // theme file name in themes/, empty = stylers.model.xml
    bool wordWrap = false;
    bool showAllChars = false;
    bool showIndentGuides = true;
    int zoom = 0;
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

    int maxRecentFiles = 10;
    std::vector<std::string> recentFiles; // most recent first

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
