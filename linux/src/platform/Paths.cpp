#include "platform/Paths.h"

#include <glib.h>

#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::platform {

fs::path executableDir()
{
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty())
        return fs::current_path(ec);
    return exe.parent_path();
}

fs::path dataDir()
{
    if (const char* env = std::getenv("NPP_GTK_DATA_DIR"); env && *env)
        return fs::path(env);
    // NPP_DATA_SUBDIR is e.g. "share/notepad-plus-plus-gtk"; the binary is in
    // <prefix>/bin both when installed and in the build tree.
    return (executableDir() / ".." / NPP_DATA_SUBDIR).lexically_normal();
}

fs::path userConfigDir()
{
    fs::path dir = fs::path(g_get_user_config_dir()) / NPP_EXE_NAME;
    std::error_code ec;
    fs::create_directories(dir, ec); // best effort; callers tolerate absence
    return dir;
}

fs::path findConfigFile(std::string_view userName, std::string_view shippedName)
{
    std::error_code ec;
    fs::path user = userConfigDir() / fs::path(userName);
    if (fs::is_regular_file(user, ec))
        return user;
    fs::path shipped = dataDir() / fs::path(shippedName);
    if (fs::is_regular_file(shipped, ec))
        return shipped;
    return {};
}

} // namespace npp::platform
