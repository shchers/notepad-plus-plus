// Import of upstream config.xml (Windows %APPDATA%\Notepad++\config.xml) —
// the subset of NppParameters::feedGUIParameters / feedScintillaParam /
// feedFileListParameters that maps onto the port's settings.
//
// The port keeps its own settings.ini; config.xml is only read: automatically
// on first run when <userConfigDir>/config.xml exists, and on demand from
// Settings > Import Notepad++ config.xml.
#pragma once

#include "platform/Settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace npp::core {

struct NppConfig {
    std::optional<bool> rememberSession, snapshot;
    std::optional<int> snapshotIntervalMs;
    std::optional<platform::SaveBackup> saveBackup;
    std::optional<std::string> saveBackupDir;
    std::optional<platform::ChangeDetection> changeDetection;
    std::optional<int> tabSize;
    std::optional<bool> replaceTabsBySpace, autoIndent;
    std::optional<bool> wordWrap, showSpaces, showEol, showIndentGuides, showWrapSymbol;
    std::optional<int> zoom;
    std::optional<std::string> theme;  // theme file name ("Zenburn.xml"), "" = default
    std::optional<int> maxRecentFiles;
    std::vector<std::string> recentFiles;
};

std::optional<NppConfig> readNppConfig(const std::filesystem::path& configXml, std::string* error = nullptr);

// Copies the values that are present. Recent files that do not exist on this
// system (Windows paths) and themes that are not installed are skipped.
// Returns how many settings were applied.
int applyNppConfig(const NppConfig& cfg, platform::Settings& s);

} // namespace npp::core
