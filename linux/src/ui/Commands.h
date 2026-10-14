// Menu commands keyed by upstream command IDs (menuCmdID.h): the GTK side of
// upstream's WM_COMMAND switch (NppCommands.cpp). Each command becomes a
// window action "win.id<ID>" (stateful when it shows a check mark), so menus,
// accelerators, shortcuts.xml, macros (type 2 steps) and nativeLang all refer
// to the same numbers as Notepad++ on Windows.
#pragma once

#include "core/PortCommands.h"
#include "core/Shortcuts.h"
#include "core/Translator.h"

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace npp::ui {

using core::kCmdImportConfig;
using core::kCmdTerminalPanel;

class CommandTable {
public:
    using Fn = std::function<void()>;
    using State = std::function<bool()>;

    void add(int id, Fn run, State checked = {});
    bool has(int id) const { return cmds_.count(id) != 0; }
    // Runs a command as if chosen from the menu. Returns false if unknown or disabled.
    bool run(int id);

    // Creates the actions on `map` for commands that have none yet.
    void install(GActionMap* map);
    // Gives a command a check mark after install() (a plugin's
    // NPPM_SETMENUITEMCHECK on an item that had none).
    void makeCheckable(int id, State checked, GActionMap* map);
    bool isCheckable(int id) const
    {
        auto it = cmds_.find(id);
        return it != cmds_.end() && it->second.checked;
    }
    void syncStates();
    void setEnabled(int id, bool enabled);
    bool isEnabled(int id) const;

    // Accelerators: menu commands from `keys`, plus `extra` (macros, Run
    // commands) mapped to their own action names.
    void applyAccels(GtkApplication* app, const core::CommandKeys& keys,
                     const std::vector<std::pair<std::string, core::KeyCombo>>& extra = {});

    // Called around every command run, for macro recording.
    std::function<void(int id)> onBeforeRun;
    std::function<void(int id)> onRun;

    static std::string actionName(int id) { return "id" + std::to_string(id); }
    static std::string detailedName(int id) { return "win." + actionName(id); }

private:
    struct Cmd {
        Fn run;
        State checked;
        GSimpleAction* action = nullptr;
    };
    static void onActivate(GSimpleAction* a, GVariant*, gpointer self);
    std::map<int, Cmd> cmds_;
    std::vector<std::string> accelActions_; // to clear on re-apply
    bool syncing_ = false;
};

// Sections of the menu bar that MainWindow fills at run time.
struct DynamicMenus {
    GMenu* recent = nullptr;
    GMenu* themes = nullptr;
    GMenu* languages = nullptr;
    GMenu* userLanguages = nullptr;
    GMenu* macros = nullptr;
    GMenu* runCommands = nullptr;
    GMenu* plugins = nullptr;
};

// Builds the main menu following upstream's Notepad_plus.rc layout. Labels
// come from `tr` (nativeLang); commands the table does not have are left out.
// The returned sections are owned by the menu.
GMenu* buildMenuBar(const core::Translator& tr, const CommandTable& cmds, DynamicMenus& dyn);

// The tab context menu (upstream tabContextMenu defaults, abridged).
GMenu* buildTabMenu(const core::Translator& tr, const CommandTable& cmds);

// Name and top-level menu of every command in the menu spec (Shortcut
// Mapper "Name" and "Category" columns), translated, without mnemonics.
struct CommandInfo {
    std::string name;
    std::string category;
};
std::map<int, CommandInfo> commandInfos(const core::Translator& tr);

// Appends `label` → action with an optional string target.
void appendItem(GMenu* m, const std::string& label, const std::string& detailedAction, const char* target = nullptr);

} // namespace npp::ui
