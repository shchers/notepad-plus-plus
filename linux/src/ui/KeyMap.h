// Windows virtual-key codes (shortcuts.xml) <-> GDK key values.
#pragma once

#include "core/KeyCombo.h"

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace npp::ui {

guint keyvalForVk(int vk);           // 0 if the key has no GDK equivalent
int vkForKeyval(guint keyval);       // 0 if unknown; letters map case-insensitively
// GTK accelerator strings ("<Primary><Shift>s"). Shift+Tab also yields the
// ISO_Left_Tab form GTK reports for it.
std::vector<std::string> accelsFor(const core::KeyCombo& k);
// Key press -> combo, using the unshifted key (Shift+1 is "Shift+1", not "!").
core::KeyCombo comboFromEvent(const GdkEventKey* ev);

} // namespace npp::ui
