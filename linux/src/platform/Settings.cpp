#include "platform/Settings.h"

#include "platform/Paths.h"

#include <glib.h>

#include <algorithm>
#include <memory>

namespace npp::platform {

namespace {
constexpr const char* kGroupView = "View";
constexpr const char* kGroupEditing = "Editing";
constexpr const char* kGroupBackup = "Backup";
constexpr const char* kGroupHistory = "History";
constexpr const char* kGroupWindow = "Window";
constexpr const char* kGroupFind = "Find";
constexpr const char* kGroupDocking = "Docking";
constexpr const char* kGroupPanels = "Panels";
using KeyFilePtr = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
} // namespace

std::filesystem::path Settings::defaultPath()
{
    return userConfigDir() / "settings.ini";
}

bool Settings::load(const std::filesystem::path& file)
{
    KeyFilePtr kf(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_file(kf.get(), file.c_str(), G_KEY_FILE_NONE, nullptr))
        return false; // first run: keep defaults

    auto getBool = [&](const char* group, const char* key, bool& out) {
        GError* e = nullptr;
        const gboolean v = g_key_file_get_boolean(kf.get(), group, key, &e);
        if (e)
            g_error_free(e);
        else
            out = v;
    };
    auto getInt = [&](const char* group, const char* key, int& out, int lo, int hi) {
        GError* e = nullptr;
        const gint v = g_key_file_get_integer(kf.get(), group, key, &e);
        if (e)
            g_error_free(e);
        else
            out = std::clamp(v, lo, hi);
    };
    auto getString = [&](const char* group, const char* key, std::string& out) {
        if (gchar* t = g_key_file_get_string(kf.get(), group, key, nullptr)) {
            out = t;
            g_free(t);
        }
    };
    getString(kGroupView, "theme", theme);
    getBool(kGroupView, "wordWrap", wordWrap);
    getString(kGroupView, "localization", localization);
    bool all = false;
    getBool(kGroupView, "showAllChars", all); // before the split into two settings
    showSpaces = showEol = all;
    getBool(kGroupView, "showSpaces", showSpaces);
    getBool(kGroupView, "showEol", showEol);
    getBool(kGroupView, "showWrapSymbol", showWrapSymbol);
    getBool(kGroupView, "showIndentGuides", showIndentGuides);
    getInt(kGroupView, "zoom", zoom, -10, 20);

    getBool(kGroupView, "hideToolbar", hideToolbar);
    getBool(kGroupView, "hideStatusBar", hideStatusBar);
    getInt(kGroupView, "wrapIndent", wrapIndent, 0, 2);
    getInt(kGroupView, "caretWidth", caretWidth, 0, 3);
    getInt(kGroupView, "caretBlinkMs", caretBlinkMs, 0, 2500);
    getBool(kGroupView, "scrollBeyondLastLine", scrollBeyondLastLine);
    getBool(kGroupView, "virtualSpace", virtualSpace);
    getBool(kGroupView, "currentLineHighlight", currentLineHighlight);
    getInt(kGroupView, "foldStyle", foldStyle, 0, 4);
    getBool(kGroupView, "showLineNumbers", showLineNumbers);
    getBool(kGroupView, "showBookmarkMargin", showBookmarkMargin);
    getInt(kGroupView, "edgeMode", edgeMode, 0, 2);
    getInt(kGroupView, "edgeColumn", edgeColumn, 1, 1000);
    getInt(kGroupEditing, "newDocEol", newDocEol, 0, 2);
    getInt(kGroupEditing, "newDocEncoding", newDocEncoding, 0, 3);
    getString(kGroupEditing, "newDocLanguage", newDocLanguage);
    getInt(kGroupEditing, "tabSize", tabSize, 1, 64);
    getBool(kGroupEditing, "replaceTabsBySpace", replaceTabsBySpace);
    getBool(kGroupEditing, "autoIndent", autoIndent);
    int detection = static_cast<int>(changeDetection);
    getInt(kGroupEditing, "changeDetection", detection, 0, 2);
    changeDetection = static_cast<ChangeDetection>(detection);

    getBool(kGroupBackup, "rememberSession", rememberSession);
    getBool(kGroupBackup, "snapshot", snapshot);
    getInt(kGroupBackup, "snapshotIntervalMs", snapshotIntervalMs, 1000, 3600 * 1000);
    int backup = static_cast<int>(saveBackup);
    getInt(kGroupBackup, "saveBackup", backup, 0, 2);
    saveBackup = static_cast<SaveBackup>(backup);
    getString(kGroupBackup, "saveBackupDir", saveBackupDir);

    getInt(kGroupHistory, "maxRecentFiles", maxRecentFiles, 0, 30); // upstream NB_MAX_LRF_FILE
    gsize n = 0;
    if (gchar** list = g_key_file_get_string_list(kf.get(), kGroupHistory, "recentFiles", &n, nullptr)) {
        recentFiles.assign(list, list + n);
        g_strfreev(list);
    }
    if (gchar** list = g_key_file_get_string_list(kf.get(), kGroupHistory, "run", &n, nullptr)) {
        runHistory.assign(list, list + std::min<gsize>(n, 10));
        g_strfreev(list);
    }

    auto getList = [&](const char* key, std::vector<std::string>& out) {
        gsize n = 0;
        if (gchar** list = g_key_file_get_string_list(kf.get(), kGroupFind, key, &n, nullptr)) {
            out.assign(list, list + std::min<gsize>(n, FindSettings::kMaxHistory));
            g_strfreev(list);
        }
    };
    getList("what", find.what);
    getList("with", find.with);
    getList("filters", find.filters);
    getList("directories", find.directories);
    getInt(kGroupFind, "mode", find.mode, 0, 2);
    getBool(kGroupFind, "matchCase", find.matchCase);
    getBool(kGroupFind, "wholeWord", find.wholeWord);
    getBool(kGroupFind, "wrap", find.wrap);
    getBool(kGroupFind, "backward", find.backward);
    getBool(kGroupFind, "dotMatchesNewline", find.dotMatchesNewline);
    getBool(kGroupFind, "inSelection", find.inSelection);
    getBool(kGroupFind, "recursive", find.recursive);
    getBool(kGroupFind, "hidden", find.hidden);
    getBool(kGroupFind, "bookmarkLine", find.bookmarkLine);
    getBool(kGroupFind, "purge", find.purge);

    if (gchar** keys = g_key_file_get_keys(kf.get(), kGroupDocking, nullptr, nullptr)) {
        for (gchar** k = keys; *k; ++k)
            getString(kGroupDocking, *k, docking[*k]);
        g_strfreev(keys);
    }
    for (std::size_t i = 0; i < projectWorkspaces.size(); ++i)
        getString(kGroupPanels, ("projectWorkspace" + std::to_string(i + 1)).c_str(), projectWorkspaces[i]);
    getBool(kGroupPanels, "functionListSorted", functionListSorted);

    getInt(kGroupWindow, "width", width, 200, 16384);
    getInt(kGroupWindow, "height", height, 150, 16384);
    getBool(kGroupWindow, "maximized", maximized);
    getInt(kGroupWindow, "splitPosition", splitPosition, -1, 16384);
    return true;
}

bool Settings::save(const std::filesystem::path& file) const
{
    KeyFilePtr kf(g_key_file_new(), &g_key_file_unref);
    g_key_file_set_string(kf.get(), kGroupView, "theme", theme.c_str());
    g_key_file_set_boolean(kf.get(), kGroupView, "wordWrap", wordWrap);
    g_key_file_set_string(kf.get(), kGroupView, "localization", localization.c_str());
    g_key_file_set_boolean(kf.get(), kGroupView, "showSpaces", showSpaces);
    g_key_file_set_boolean(kf.get(), kGroupView, "showEol", showEol);
    g_key_file_set_boolean(kf.get(), kGroupView, "showWrapSymbol", showWrapSymbol);
    g_key_file_set_boolean(kf.get(), kGroupView, "showIndentGuides", showIndentGuides);
    g_key_file_set_integer(kf.get(), kGroupView, "zoom", zoom);

    g_key_file_set_boolean(kf.get(), kGroupView, "hideToolbar", hideToolbar);
    g_key_file_set_boolean(kf.get(), kGroupView, "hideStatusBar", hideStatusBar);
    g_key_file_set_integer(kf.get(), kGroupView, "wrapIndent", wrapIndent);
    g_key_file_set_integer(kf.get(), kGroupView, "caretWidth", caretWidth);
    g_key_file_set_integer(kf.get(), kGroupView, "caretBlinkMs", caretBlinkMs);
    g_key_file_set_boolean(kf.get(), kGroupView, "scrollBeyondLastLine", scrollBeyondLastLine);
    g_key_file_set_boolean(kf.get(), kGroupView, "virtualSpace", virtualSpace);
    g_key_file_set_boolean(kf.get(), kGroupView, "currentLineHighlight", currentLineHighlight);
    g_key_file_set_integer(kf.get(), kGroupView, "foldStyle", foldStyle);
    g_key_file_set_boolean(kf.get(), kGroupView, "showLineNumbers", showLineNumbers);
    g_key_file_set_boolean(kf.get(), kGroupView, "showBookmarkMargin", showBookmarkMargin);
    g_key_file_set_integer(kf.get(), kGroupView, "edgeMode", edgeMode);
    g_key_file_set_integer(kf.get(), kGroupView, "edgeColumn", edgeColumn);
    g_key_file_set_integer(kf.get(), kGroupEditing, "newDocEol", newDocEol);
    g_key_file_set_integer(kf.get(), kGroupEditing, "newDocEncoding", newDocEncoding);
    g_key_file_set_string(kf.get(), kGroupEditing, "newDocLanguage", newDocLanguage.c_str());
    g_key_file_set_integer(kf.get(), kGroupEditing, "tabSize", tabSize);
    g_key_file_set_boolean(kf.get(), kGroupEditing, "replaceTabsBySpace", replaceTabsBySpace);
    g_key_file_set_boolean(kf.get(), kGroupEditing, "autoIndent", autoIndent);
    g_key_file_set_integer(kf.get(), kGroupEditing, "changeDetection", static_cast<int>(changeDetection));

    g_key_file_set_boolean(kf.get(), kGroupBackup, "rememberSession", rememberSession);
    g_key_file_set_boolean(kf.get(), kGroupBackup, "snapshot", snapshot);
    g_key_file_set_integer(kf.get(), kGroupBackup, "snapshotIntervalMs", snapshotIntervalMs);
    g_key_file_set_integer(kf.get(), kGroupBackup, "saveBackup", static_cast<int>(saveBackup));
    g_key_file_set_string(kf.get(), kGroupBackup, "saveBackupDir", saveBackupDir.c_str());

    g_key_file_set_integer(kf.get(), kGroupHistory, "maxRecentFiles", maxRecentFiles);
    std::vector<const gchar*> list;
    for (const std::string& f : recentFiles)
        list.push_back(f.c_str());
    g_key_file_set_string_list(kf.get(), kGroupHistory, "recentFiles", list.data(), list.size());
    list.clear();
    for (const std::string& r : runHistory)
        list.push_back(r.c_str());
    g_key_file_set_string_list(kf.get(), kGroupHistory, "run", list.data(), list.size());

    auto setList = [&](const char* key, const std::vector<std::string>& in) {
        std::vector<const gchar*> v;
        for (const std::string& s : in)
            v.push_back(s.c_str());
        g_key_file_set_string_list(kf.get(), kGroupFind, key, v.data(), v.size());
    };
    setList("what", find.what);
    setList("with", find.with);
    setList("filters", find.filters);
    setList("directories", find.directories);
    g_key_file_set_integer(kf.get(), kGroupFind, "mode", find.mode);
    g_key_file_set_boolean(kf.get(), kGroupFind, "matchCase", find.matchCase);
    g_key_file_set_boolean(kf.get(), kGroupFind, "wholeWord", find.wholeWord);
    g_key_file_set_boolean(kf.get(), kGroupFind, "wrap", find.wrap);
    g_key_file_set_boolean(kf.get(), kGroupFind, "backward", find.backward);
    g_key_file_set_boolean(kf.get(), kGroupFind, "dotMatchesNewline", find.dotMatchesNewline);
    g_key_file_set_boolean(kf.get(), kGroupFind, "inSelection", find.inSelection);
    g_key_file_set_boolean(kf.get(), kGroupFind, "recursive", find.recursive);
    g_key_file_set_boolean(kf.get(), kGroupFind, "hidden", find.hidden);
    g_key_file_set_boolean(kf.get(), kGroupFind, "bookmarkLine", find.bookmarkLine);
    g_key_file_set_boolean(kf.get(), kGroupFind, "purge", find.purge);

    for (const auto& [k, v] : docking)
        g_key_file_set_string(kf.get(), kGroupDocking, k.c_str(), v.c_str());
    for (std::size_t i = 0; i < projectWorkspaces.size(); ++i)
        g_key_file_set_string(kf.get(), kGroupPanels, ("projectWorkspace" + std::to_string(i + 1)).c_str(), projectWorkspaces[i].c_str());
    g_key_file_set_boolean(kf.get(), kGroupPanels, "functionListSorted", functionListSorted);

    g_key_file_set_integer(kf.get(), kGroupWindow, "width", width);
    g_key_file_set_integer(kf.get(), kGroupWindow, "height", height);
    g_key_file_set_boolean(kf.get(), kGroupWindow, "maximized", maximized);
    g_key_file_set_integer(kf.get(), kGroupWindow, "splitPosition", splitPosition);
    return g_key_file_save_to_file(kf.get(), file.c_str(), nullptr);
}

} // namespace npp::platform
