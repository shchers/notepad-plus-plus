// Running inside Flatpak or Snap. Run commands and "terminal here" start
// programs on the user's system, which a sandbox hides: under Flatpak they go
// through `flatpak-spawn --host` (the manifest grants it); a strictly
// confined Snap cannot reach them at all.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace npp::platform {

enum class Sandbox { None, Flatpak, Snap };

Sandbox currentSandbox();

// The argument vector that runs `argv` on the host from `cwd`.
std::vector<std::string> hostCommand(const std::vector<std::string>& argv, const std::filesystem::path& cwd, Sandbox s);

// A shell command line that starts the first terminal emulator found
// (Debian's x-terminal-emulator, then common ones), for hostCommand().
std::vector<std::string> terminalCommand();

} // namespace npp::platform
