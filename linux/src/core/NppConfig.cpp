#include "core/NppConfig.h"

#include "platform/Paths.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cstring>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

// Upstream getBoolAttribute: "yes"/"no", or "show"/"hide" for view toggles.
std::optional<bool> boolAttr(const pugi::xml_node& n, const char* name)
{
    const pugi::xml_attribute a = n.attribute(name);
    if (!a)
        return std::nullopt;
    const std::string_view v = a.as_string();
    if (v == "yes" || v == "show")
        return true;
    if (v == "no" || v == "hide")
        return false;
    return std::nullopt;
}

std::optional<bool> boolText(const pugi::xml_node& n)
{
    const std::string_view v = n.text().as_string();
    if (v == "yes")
        return true;
    if (v == "no")
        return false;
    return std::nullopt;
}

// Theme paths in config.xml are Windows paths; keep the file name only.
std::string themeFileName(std::string_view path)
{
    const std::size_t slash = path.find_last_of("\\/");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

bool themeInstalled(const std::string& name)
{
    std::error_code ec;
    return fs::is_regular_file(platform::userConfigDir() / "themes" / name, ec) ||
           fs::is_regular_file(platform::dataDir() / "themes" / name, ec);
}

} // namespace

std::optional<NppConfig> readNppConfig(const fs::path& configXml, std::string* error)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result res = doc.load_file(configXml.c_str());
    if (!res) {
        if (error)
            *error = configXml.string() + ": " + res.description();
        return std::nullopt;
    }
    const pugi::xml_node root = doc.child("NotepadPlus");
    if (!root || !root.child("GUIConfigs")) {
        if (error)
            *error = configXml.string() + ": not a Notepad++ config.xml";
        return std::nullopt;
    }

    NppConfig c;
    for (const pugi::xml_node g : root.child("GUIConfigs").children("GUIConfig")) {
        const char* nm = g.attribute("name").as_string();
        if (!std::strcmp(nm, "RememberLastSession")) {
            c.rememberSession = boolText(g);
        } else if (!std::strcmp(nm, "Backup")) {
            if (const pugi::xml_attribute a = g.attribute("action"); a && a.as_int() >= 0 && a.as_int() <= 2)
                c.saveBackup = static_cast<platform::SaveBackup>(a.as_int());
            if (boolAttr(g, "useCustumDir").value_or(false)) // sic, upstream spelling
                c.saveBackupDir = g.attribute("dir").as_string();
            c.snapshot = boolAttr(g, "isSnapshotMode");
            if (const pugi::xml_attribute a = g.attribute("snapshotBackupTiming"); a && a.as_int() >= 1000)
                c.snapshotIntervalMs = a.as_int();
        } else if (!std::strcmp(nm, "Auto-detection")) {
            const std::string_view v = g.text().as_string();
            if (v == "no")
                c.changeDetection = platform::ChangeDetection::Off;
            else if (v.find("auto") != std::string_view::npos)
                c.changeDetection = platform::ChangeDetection::AutoUpdate;
            else if (!v.empty())
                c.changeDetection = platform::ChangeDetection::Notify;
        } else if (!std::strcmp(nm, "TabSetting")) {
            if (const int sz = g.attribute("size").as_int(0); sz > 0 && sz <= 64)
                c.tabSize = sz;
            c.replaceTabsBySpace = boolAttr(g, "replaceBySpace");
        } else if (!std::strcmp(nm, "MaintainIndent")) {
            const std::string_view v = g.text().as_string();
            if (!v.empty())
                c.autoIndent = v != "0" && v != "no";
        } else if (!std::strcmp(nm, "ScintillaPrimaryView")) {
            c.wordWrap = boolAttr(g, "Wrap");
            c.showIndentGuides = boolAttr(g, "indentGuideLine");
            const auto ws = boolAttr(g, "whiteSpaceShow"), eol = boolAttr(g, "eolShow");
            if (ws && eol)
                c.showAllChars = *ws && *eol;
            if (g.attribute("zoom"))
                c.zoom = g.attribute("zoom").as_int();
        } else if (!std::strcmp(nm, "DarkMode")) {
            // Upstream picks the theme for the current mode.
            const bool dark = boolAttr(g, "enable").value_or(false);
            const char* attr = dark ? "darkThemeName" : "lightThemeName";
            if (g.attribute(attr))
                c.theme = themeFileName(g.attribute(attr).as_string());
        } else if (!std::strcmp(nm, "stylerTheme")) { // older config.xml
            if (!c.theme && g.attribute("path"))
                c.theme = themeFileName(g.attribute("path").as_string());
        }
    }

    if (const pugi::xml_node h = root.child("History")) {
        if (h.attribute("nbMaxFile"))
            c.maxRecentFiles = std::clamp(h.attribute("nbMaxFile").as_int(10), 0, 30);
        for (const pugi::xml_node f : h.children("File"))
            if (const char* fn = f.attribute("filename").as_string(); *fn)
                c.recentFiles.emplace_back(fn);
    }
    return c;
}

int applyNppConfig(const NppConfig& c, platform::Settings& s)
{
    int n = 0;
    auto set = [&n](auto& dst, const auto& src) {
        if (src) {
            dst = *src;
            ++n;
        }
    };
    set(s.rememberSession, c.rememberSession);
    set(s.snapshot, c.snapshot);
    set(s.snapshotIntervalMs, c.snapshotIntervalMs);
    set(s.saveBackup, c.saveBackup);
    set(s.saveBackupDir, c.saveBackupDir);
    set(s.changeDetection, c.changeDetection);
    set(s.tabSize, c.tabSize);
    set(s.replaceTabsBySpace, c.replaceTabsBySpace);
    set(s.autoIndent, c.autoIndent);
    set(s.wordWrap, c.wordWrap);
    set(s.showAllChars, c.showAllChars);
    set(s.showIndentGuides, c.showIndentGuides);
    set(s.zoom, c.zoom);
    set(s.maxRecentFiles, c.maxRecentFiles);
    if (s.saveBackupDir.find('\\') != std::string::npos)
        s.saveBackupDir.clear(); // a Windows directory means nothing here
    if (c.theme) {
        if (c.theme->empty() || *c.theme == "stylers.model.xml" || *c.theme == "stylers.xml") {
            s.theme.clear();
            ++n;
        } else if (themeInstalled(*c.theme)) {
            s.theme = *c.theme;
            ++n;
        }
    }
    for (auto it = c.recentFiles.rbegin(); it != c.recentFiles.rend(); ++it) {
        std::error_code ec;
        if (fs::path(*it).is_absolute() && fs::is_regular_file(*it, ec)) {
            std::erase(s.recentFiles, *it);
            s.recentFiles.insert(s.recentFiles.begin(), *it);
            ++n;
        }
    }
    if (s.maxRecentFiles >= 0 && s.recentFiles.size() > static_cast<std::size_t>(s.maxRecentFiles))
        s.recentFiles.resize(static_cast<std::size_t>(s.maxRecentFiles));
    return n;
}

} // namespace npp::core
