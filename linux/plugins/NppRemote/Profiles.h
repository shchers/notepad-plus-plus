// NppRemote — connection profiles and plugin settings, kept in
// <plugins config>/NppRemote.ini (GKeyFile), readable by the user only:
// saved passwords are in it as plain text.
#pragma once

#include "RemoteClient.h"

#include <string>
#include <vector>

namespace nppremote {

struct Settings {
    std::vector<Profile> profiles;
    bool showLog = true;
    int logPanePosition = 0; // 0: default
};

bool loadSettings(const std::string& file, Settings& s);
bool saveSettings(const std::string& file, const Settings& s, std::string& error);

// A name not used by another profile ("New profile", "New profile 2", ...).
std::string uniqueProfileName(const std::vector<Profile>& profiles, const std::string& base);

} // namespace nppremote
