// Plugin folders on disk — the file side of upstream's Plugins Admin
// (WinControls/PluginsAdmin): which plugins are installed, and installing,
// removing, deactivating and activating them. As upstream, a plugin is
// <root>/<Name>/<Name>.so, and a deactivated one is moved to
// <root>/disabled/<Name>/. Changes take effect at the next start.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace npp::core {

struct PluginFolder {
    std::string name;            // folder and module stem
    std::filesystem::path root;  // the plugins directory
    std::filesystem::path dir;   // <root>/<Name> or <root>/disabled/<Name>
    std::filesystem::path file;  // dir/<Name>.so
    bool disabled = false;
    bool writable = false;       // the user may change it (not a system directory)
    // A <Name>.so directly in the plugins directory: never loaded (it needs a
    // folder of its own); dir is the plugins directory then.
    bool misplaced = false;
    std::string version;         // NPPGTK_PLUGIN_VERSION, "" if none
};

// The version a plugin file declares with NPPGTK_PLUGIN_VERSION (NppGtk.h),
// read from the file without loading it; "" if none.
std::string pluginVersion(const std::filesystem::path& so);

// The plugins in each root, in root then name order, active and deactivated,
// and the misplaced .so files.
std::vector<PluginFolder> listPlugins(const std::vector<std::filesystem::path>& roots);

// Whether files can be added under `root` (it, or the nearest existing
// directory above it, is writable).
bool canModify(const std::filesystem::path& root);

bool deactivatePlugin(const PluginFolder& p, std::string* error = nullptr);
bool activatePlugin(const PluginFolder& p, std::string* error = nullptr);
bool removePlugin(const PluginFolder& p, std::string* error = nullptr);

// Installs into `root`, replacing a plugin of the same name (active or
// deactivated): a <Name>.so file (upstream's Import plugin(s), which takes
// a .dll), a folder holding <Name>.so, or a .zip with <Name>.so at its top or
// in a <Name>/ folder (how upstream's plugins are published). Returns the
// plugin's name.
std::optional<std::string> installPlugin(const std::filesystem::path& source, const std::filesystem::path& root,
                                         std::string* error = nullptr);

} // namespace npp::core
