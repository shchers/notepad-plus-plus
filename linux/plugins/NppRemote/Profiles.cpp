#include "Profiles.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace nppremote {

namespace {

std::string getString(GKeyFile* kf, const char* group, const char* key)
{
    gchar* v = g_key_file_get_string(kf, group, key, nullptr);
    std::string out = v ? v : "";
    g_free(v);
    return out;
}

int getInt(GKeyFile* kf, const char* group, const char* key, int fallback)
{
    GError* err = nullptr;
    const int v = g_key_file_get_integer(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return fallback;
    }
    return v;
}

bool getBool(GKeyFile* kf, const char* group, const char* key, bool fallback)
{
    GError* err = nullptr;
    const gboolean v = g_key_file_get_boolean(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return fallback;
    }
    return v;
}

} // namespace

bool loadSettings(const std::string& file, Settings& s)
{
    s = Settings{};
    GKeyFile* kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, file.c_str(), G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        return false;
    }
    s.showLog = getBool(kf, "General", "ShowLog", true);
    s.logPanePosition = getInt(kf, "General", "LogPanePosition", 0);
    gsize n = 0;
    gchar** groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; ++i) {
        const char* g = groups[i];
        if (std::strncmp(g, "Profile ", 8) != 0)
            continue;
        Profile p;
        p.name = getString(kf, g, "Name");
        if (p.name.empty())
            continue;
        p.protocol = getString(kf, g, "Protocol") == "ftp" ? Protocol::Ftp : Protocol::Sftp;
        p.host = getString(kf, g, "Host");
        p.port = getInt(kf, g, "Port", 0);
        p.user = getString(kf, g, "User");
        p.password = getString(kf, g, "Password");
        p.savePassword = !p.password.empty();
        p.initialDir = getString(kf, g, "InitialDir");
        p.activeMode = getBool(kf, g, "ActiveMode", false);
        p.identityFile = getString(kf, g, "IdentityFile");
        p.sftpOptions = getString(kf, g, "SftpOptions");
        p.timeout = getInt(kf, g, "Timeout", 30);
        if (p.timeout < 5)
            p.timeout = 5;
        s.profiles.push_back(std::move(p));
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return true;
}

bool saveSettings(const std::string& file, const Settings& s, std::string& error)
{
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_boolean(kf, "General", "ShowLog", s.showLog);
    g_key_file_set_integer(kf, "General", "LogPanePosition", s.logPanePosition);
    int i = 0;
    for (const Profile& p : s.profiles) {
        const std::string g = "Profile " + std::to_string(++i);
        g_key_file_set_string(kf, g.c_str(), "Name", p.name.c_str());
        g_key_file_set_string(kf, g.c_str(), "Protocol", p.protocol == Protocol::Ftp ? "ftp" : "sftp");
        g_key_file_set_string(kf, g.c_str(), "Host", p.host.c_str());
        g_key_file_set_integer(kf, g.c_str(), "Port", p.port);
        g_key_file_set_string(kf, g.c_str(), "User", p.user.c_str());
        if (p.savePassword && !p.password.empty())
            g_key_file_set_string(kf, g.c_str(), "Password", p.password.c_str());
        g_key_file_set_string(kf, g.c_str(), "InitialDir", p.initialDir.c_str());
        g_key_file_set_boolean(kf, g.c_str(), "ActiveMode", p.activeMode);
        g_key_file_set_string(kf, g.c_str(), "IdentityFile", p.identityFile.c_str());
        g_key_file_set_string(kf, g.c_str(), "SftpOptions", p.sftpOptions.c_str());
        g_key_file_set_integer(kf, g.c_str(), "Timeout", p.timeout);
    }
    gsize len = 0;
    gchar* data = g_key_file_to_data(kf, &len, nullptr);
    g_key_file_free(kf);

    // Written to a new file, mode 0600 from the start, then moved over.
    const std::string tmp = file + ".tmp";
    bool ok = false;
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ok = ::write(fd, data, len) == static_cast<ssize_t>(len);
        ok = ::close(fd) == 0 && ok;
        ok = ok && ::rename(tmp.c_str(), file.c_str()) == 0;
    }
    if (!ok) {
        error = file + ": " + std::strerror(errno);
        ::unlink(tmp.c_str());
    }
    g_free(data);
    return ok;
}

std::string uniqueProfileName(const std::vector<Profile>& profiles, const std::string& base)
{
    for (int n = 1;; ++n) {
        const std::string name = n == 1 ? base : base + " " + std::to_string(n);
        bool used = false;
        for (const Profile& p : profiles)
            used = used || p.name == name;
        if (!used)
            return name;
    }
}

} // namespace nppremote
