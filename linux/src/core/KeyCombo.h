// Keyboard shortcut as Notepad++ stores it: Ctrl/Alt/Shift + a Windows
// virtual-key code (shortcuts.xml "Key" attribute, upstream KeyCombo).
// Conversion to and from GDK key values lives in ui/KeyMap.
#pragma once

#include <string>
#include <vector>

namespace npp::core {

struct KeyCombo {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    int vk = 0; // 0 = no shortcut

    bool empty() const { return vk == 0; }
    bool operator==(const KeyCombo&) const = default;
    // Upstream Shortcut::toString(): "Ctrl+Alt+Shift+F3", "" when empty.
    std::string toString() const;
};

// Upstream namedKeyArray: display name of a virtual key ("Page up", "F3",
// "Num +"), empty if unknown.
std::string vkName(int vk);
// Virtual keys offered by the shortcut editor, in upstream order.
const std::vector<int>& assignableKeys();

} // namespace npp::core
