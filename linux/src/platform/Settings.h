// Persistent user settings — the port's stand-in for upstream config.xml
// (NppGUI). Stored as an INI file via GKeyFile at
// $XDG_CONFIG_HOME/notepad-plus-plus-gtk/settings.ini.
#pragma once

#include <filesystem>
#include <string>

namespace npp::platform {

struct Settings {
    std::string theme;          // theme file name in themes/, empty = stylers.model.xml
    bool wordWrap = false;
    bool showAllChars = false;
    bool showIndentGuides = true;
    int zoom = 0;
    int width = 1100;
    int height = 750;
    bool maximized = false;

    static std::filesystem::path defaultPath();
    void load(const std::filesystem::path& file = defaultPath());
    bool save(const std::filesystem::path& file = defaultPath()) const;
};

} // namespace npp::platform
