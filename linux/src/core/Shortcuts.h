// shortcuts.xml — upstream's file for customised menu shortcuts, macros, Run
// commands, plugin and Scintilla key bindings (NppParameters::feedShortcut,
// feedMacros, feedUserCmds, writeShortcuts). Read and written in the same
// format; the PluginCommands and ScintillaKeys sections are kept verbatim.
//
// CommandKeys holds the effective shortcut of every menu command: upstream's
// defaults (winKeyDefs, generated) with shortcuts.xml's changes on top.
#pragma once

#include "core/KeyCombo.h"
#include "core/Macro.h"

#include <filesystem>
#include <string>
#include <vector>

namespace npp::core {

// One row of the Shortcut Mapper "Main menu" tab. A command can have several
// rows (upstream defaults give Cut both Ctrl+X and Shift+Del); `nth` numbers
// them, as the "nth" attribute in shortcuts.xml.
struct CommandKey {
    int id = 0;
    int nth = 0;
    KeyCombo key;
    bool operator==(const CommandKey&) const = default;
};

struct SavedMacro {
    std::string name;
    KeyCombo key;
    std::string folder;
    Macro steps;
};

struct UserCommand {
    std::string name;
    KeyCombo key;
    std::string folder;
    std::string command;
};

class ShortcutsFile {
public:
    std::vector<CommandKey> commands; // changed menu shortcuts only
    std::vector<SavedMacro> macros;
    std::vector<UserCommand> userCommands;

    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;

private:
    std::string pluginCommandsXml_;
    std::string scintillaKeysXml_;
};

class CommandKeys {
public:
    CommandKeys(); // upstream defaults

    const std::vector<CommandKey>& all() const { return keys_; }
    std::vector<KeyCombo> keysOf(int id) const;
    // Name for commands that have no menu item of their own, else nullptr.
    static const char* defaultName(int id);

    void set(int id, int nth, const KeyCombo& key);
    void apply(const std::vector<CommandKey>& changes);
    std::vector<CommandKey> changes() const; // what differs from the defaults
    // Commands (other than `except`) already bound to `key`.
    std::vector<CommandKey> conflicts(const KeyCombo& key, int exceptId = 0, int exceptNth = -1) const;

private:
    std::vector<CommandKey> defaults_;
    std::vector<CommandKey> keys_;
};

} // namespace npp::core
