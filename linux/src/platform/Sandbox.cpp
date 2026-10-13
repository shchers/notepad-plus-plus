#include "platform/Sandbox.h"

#include <cstdlib>
#include <system_error>

namespace npp::platform {

Sandbox currentSandbox()
{
    std::error_code ec;
    if (std::filesystem::exists("/.flatpak-info", ec))
        return Sandbox::Flatpak;
    if (const char* snap = std::getenv("SNAP"); snap && *snap && std::getenv("SNAP_NAME"))
        return Sandbox::Snap;
    return Sandbox::None;
}

std::vector<std::string> hostCommand(const std::vector<std::string>& argv, const std::filesystem::path& cwd, Sandbox s)
{
    if (s != Sandbox::Flatpak)
        return argv;
    std::vector<std::string> out = {"flatpak-spawn", "--host"};
    if (!cwd.empty())
        out.push_back("--directory=" + cwd.string());
    out.push_back("--");
    out.insert(out.end(), argv.begin(), argv.end());
    return out;
}

std::vector<std::string> terminalCommand()
{
    return {"sh", "-c",
            "for t in x-terminal-emulator gnome-terminal konsole xfce4-terminal mate-terminal kitty alacritty xterm; do "
            "command -v \"$t\" >/dev/null 2>&1 && exec \"$t\"; done; exit 127"};
}

} // namespace npp::platform
