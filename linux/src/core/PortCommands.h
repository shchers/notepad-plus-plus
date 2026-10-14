// Command IDs of the port's own menu commands, which have no upstream
// counterpart: beyond upstream's ranges (menuCmdID.h), so they never clash.
#pragma once

namespace npp::core {

inline constexpr int kCmdImportConfig = 49501;  // Settings > Import > Import Notepad++ config.xml...
inline constexpr int kCmdTerminalPanel = 49502; // View > Terminal

} // namespace npp::core
