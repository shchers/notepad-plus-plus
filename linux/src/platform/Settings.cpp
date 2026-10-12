#include "platform/Settings.h"

#include "platform/Paths.h"

#include <glib.h>

#include <memory>

namespace npp::platform {

namespace {
constexpr const char* kGroupView = "View";
constexpr const char* kGroupWindow = "Window";
using KeyFilePtr = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
} // namespace

std::filesystem::path Settings::defaultPath()
{
    return userConfigDir() / "settings.ini";
}

void Settings::load(const std::filesystem::path& file)
{
    KeyFilePtr kf(g_key_file_new(), &g_key_file_unref);
    if (!g_key_file_load_from_file(kf.get(), file.c_str(), G_KEY_FILE_NONE, nullptr))
        return; // first run: keep defaults

    auto getBool = [&](const char* group, const char* key, bool& out) {
        GError* e = nullptr;
        const gboolean v = g_key_file_get_boolean(kf.get(), group, key, &e);
        if (e)
            g_error_free(e);
        else
            out = v;
    };
    auto getInt = [&](const char* group, const char* key, int& out) {
        GError* e = nullptr;
        const gint v = g_key_file_get_integer(kf.get(), group, key, &e);
        if (e)
            g_error_free(e);
        else
            out = v;
    };
    if (gchar* t = g_key_file_get_string(kf.get(), kGroupView, "theme", nullptr)) {
        theme = t;
        g_free(t);
    }
    getBool(kGroupView, "wordWrap", wordWrap);
    getBool(kGroupView, "showAllChars", showAllChars);
    getBool(kGroupView, "showIndentGuides", showIndentGuides);
    getInt(kGroupView, "zoom", zoom);
    getInt(kGroupWindow, "width", width);
    getInt(kGroupWindow, "height", height);
    getBool(kGroupWindow, "maximized", maximized);
}

bool Settings::save(const std::filesystem::path& file) const
{
    KeyFilePtr kf(g_key_file_new(), &g_key_file_unref);
    g_key_file_set_string(kf.get(), kGroupView, "theme", theme.c_str());
    g_key_file_set_boolean(kf.get(), kGroupView, "wordWrap", wordWrap);
    g_key_file_set_boolean(kf.get(), kGroupView, "showAllChars", showAllChars);
    g_key_file_set_boolean(kf.get(), kGroupView, "showIndentGuides", showIndentGuides);
    g_key_file_set_integer(kf.get(), kGroupView, "zoom", zoom);
    g_key_file_set_integer(kf.get(), kGroupWindow, "width", width);
    g_key_file_set_integer(kf.get(), kGroupWindow, "height", height);
    g_key_file_set_boolean(kf.get(), kGroupWindow, "maximized", maximized);
    return g_key_file_save_to_file(kf.get(), file.c_str(), nullptr);
}

} // namespace npp::platform
