// Macro steps in upstream's format (shortcut.h recordedMacroStep), so
// macros recorded here and in Notepad++ for Windows are interchangeable via
// shortcuts.xml.
#pragma once

#include <Scintilla.h>

#include <cstdint>
#include <string>
#include <vector>

namespace npp::core {

struct MacroStep {
    // Upstream MacroTypeIndex.
    enum Type { UseLParam = 0, UseSParam = 1, MenuCommand = 2, SavedSearch = 3 };

    int type = UseLParam;
    int message = 0;          // SCI_* message, command ID (MenuCommand) or Find dialog ID (SavedSearch)
    std::int64_t wParam = 0;
    std::int64_t lParam = 0;
    std::string sParam;

    bool operator==(const MacroStep&) const = default;

    // From SCN_MACRORECORD: messages whose lParam is a string keep a copy of it.
    static MacroStep fromScintilla(int message, uptr_t wParam, sptr_t lParam);
    static MacroStep command(int id) { return {MenuCommand, 0, id, 0, {}}; }
    static MacroStep search(int what, std::int64_t intValue) { return {SavedSearch, what, 0, intValue, {}}; }
    static MacroStep search(int what, std::string s) { return {SavedSearch, what, 0, 0, std::move(s)}; }

    // Upstream recordedMacroStep::isMacroable(): only these Scintilla
    // messages are replayed (display changes and the like are skipped).
    bool isPlayableScintilla() const;
};

using Macro = std::vector<MacroStep>;

// Upstream NppCommands.cpp list of menu commands that are recorded.
bool isRecordableCommand(int id);

// Upstream NppParameters::getActions: older shortcuts.xml files store typed
// newlines as SCI_REPLACESEL "\r"/"\n"/"\r\n"; turn them into SCI_NEWLINE.
Macro normaliseNewlines(const Macro& in);

} // namespace npp::core
