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
    getBool(kGroupView, "showAllChars", showAllChars);
    getBool(kGroupView, "showIndentGuides", showIndentGuides);
    getInt(kGroupView, "zoom", zoom, -10, 20);

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
    g_key_file_set_boolean(kf.get(), kGroupView, "showAllChars", showAllChars);
    g_key_file_set_boolean(kf.get(), kGroupView, "showIndentGuides", showIndentGuides);
    g_key_file_set_integer(kf.get(), kGroupView, "zoom", zoom);

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

    g_key_file_set_integer(kf.get(), kGroupWindow, "width", width);
    g_key_file_set_integer(kf.get(), kGroupWindow, "height", height);
    g_key_file_set_boolean(kf.get(), kGroupWindow, "maximized", maximized);
    g_key_file_set_integer(kf.get(), kGroupWindow, "splitPosition", splitPosition);
    return g_key_file_save_to_file(kf.get(), file.c_str(), nullptr);
}

} // namespace npp::platform
