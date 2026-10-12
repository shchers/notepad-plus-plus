// The "Shortcut" dialog — upstream Shortcut::run_dlgProc: a name, Ctrl /
// Alt / Shift and a key from upstream's list. Used to save a macro or a Run
// command and by the Shortcut Mapper. Pressing a key combination while the
// dialog is open fills the fields, as a convenience.
#pragma once

#include "core/KeyCombo.h"
#include "core/Translator.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace npp::ui {

struct ShortcutEdit {
    std::string name;
    core::KeyCombo key;
    bool nameEditable = true;
    // Returns a warning to show for a key, e.g. what it conflicts with ("" = none).
    std::function<std::string(const core::KeyCombo&)> conflictInfo;
};

// Returns false when cancelled.
bool runShortcutDialog(GtkWindow* parent, const core::Translator& tr, ShortcutEdit& edit);

} // namespace npp::ui
