// Filesystem locations — replaces Win32 GetModuleFileName / SHGetKnownFolderPath
// / %APPDATA% logic in upstream NppParameters.
#pragma once

#include <filesystem>
#include <string_view>

namespace npp::platform {

// Directory containing the running executable (resolved from /proc/self/exe).
std::filesystem::path executableDir();

// Read-only application data (langs.model.xml, stylers.model.xml, ...).
// Order: $NPP_GTK_DATA_DIR, then <exe>/../share/notepad-plus-plus-gtk.
std::filesystem::path dataDir();

// Per-user writable config: $XDG_CONFIG_HOME/notepad-plus-plus-gtk
// (defaults to ~/.config/...). Created on first call.
std::filesystem::path userConfigDir();

// Resolve a config file the way Notepad++ does: a user copy overrides the
// shipped one. Looks for <userConfigDir>/<name>, then <dataDir>/<fallback>.
// Returns empty path if neither exists.
std::filesystem::path findConfigFile(std::string_view userName, std::string_view shippedName);

} // namespace npp::platform
