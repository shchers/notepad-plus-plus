#include "ui/KeyMap.h"

#include <gdk/gdkkeysyms.h>

#include <utility>

namespace npp::ui {

namespace {

const std::vector<std::pair<int, guint>>& table()
{
    static const std::vector<std::pair<int, guint>> t = [] {
        std::vector<std::pair<int, guint>> v = {
            {0x08, GDK_KEY_BackSpace}, {0x09, GDK_KEY_Tab},       {0x0D, GDK_KEY_Return},   {0x1B, GDK_KEY_Escape},
            {0x20, GDK_KEY_space},     {0x21, GDK_KEY_Page_Up},   {0x22, GDK_KEY_Page_Down}, {0x23, GDK_KEY_End},
            {0x24, GDK_KEY_Home},      {0x25, GDK_KEY_Left},      {0x26, GDK_KEY_Up},        {0x27, GDK_KEY_Right},
            {0x28, GDK_KEY_Down},      {0x2D, GDK_KEY_Insert},    {0x2E, GDK_KEY_Delete},
            {0x6A, GDK_KEY_KP_Multiply}, {0x6B, GDK_KEY_KP_Add}, {0x6D, GDK_KEY_KP_Subtract},
            {0x6E, GDK_KEY_KP_Decimal}, {0x6F, GDK_KEY_KP_Divide},
            {0xC0, GDK_KEY_grave},     {0xBD, GDK_KEY_minus},     {0xBB, GDK_KEY_equal},     {0xDB, GDK_KEY_bracketleft},
            {0xDD, GDK_KEY_bracketright}, {0xBA, GDK_KEY_semicolon}, {0xDE, GDK_KEY_apostrophe},
            {0xDC, GDK_KEY_backslash}, {0xBC, GDK_KEY_comma},     {0xBE, GDK_KEY_period},    {0xBF, GDK_KEY_slash},
            {0xE2, GDK_KEY_less},
        };
        for (int i = 0; i < 10; ++i) {
            v.emplace_back(0x30 + i, GDK_KEY_0 + i);
            v.emplace_back(0x60 + i, GDK_KEY_KP_0 + i);
        }
        for (int i = 0; i < 26; ++i)
            v.emplace_back(0x41 + i, GDK_KEY_a + i);
        for (int i = 0; i < 24; ++i)
            v.emplace_back(0x70 + i, GDK_KEY_F1 + i);
        return v;
    }();
    return t;
}

} // namespace

guint keyvalForVk(int vk)
{
    for (const auto& [k, kv] : table())
        if (k == vk)
            return kv;
    return 0;
}

int vkForKeyval(guint keyval)
{
    keyval = gdk_keyval_to_lower(keyval);
    if (keyval == GDK_KEY_ISO_Left_Tab)
        keyval = GDK_KEY_Tab;
    if (keyval == GDK_KEY_KP_Enter)
        keyval = GDK_KEY_Return;
    for (const auto& [k, kv] : table())
        if (kv == keyval)
            return k;
    return 0;
}

std::vector<std::string> accelsFor(const core::KeyCombo& k)
{
    const guint kv = keyvalForVk(k.vk);
    if (!kv)
        return {};
    std::string mods;
    if (k.ctrl)
        mods += "<Primary>";
    if (k.alt)
        mods += "<Alt>";
    if (k.shift)
        mods += "<Shift>";
    std::vector<std::string> out{mods + gdk_keyval_name(kv)};
    if (kv == GDK_KEY_Tab && k.shift)
        out.push_back(mods + "ISO_Left_Tab");
    return out;
}

core::KeyCombo comboFromEvent(const GdkEventKey* ev)
{
    core::KeyCombo c;
    c.ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    c.alt = (ev->state & GDK_MOD1_MASK) != 0;
    c.shift = (ev->state & GDK_SHIFT_MASK) != 0;
    // The key on level 0 of the pressed key (ignores what Shift produces).
    guint kv = ev->keyval;
    GdkKeymap* km = gdk_keymap_get_for_display(gdk_window_get_display(ev->window ? ev->window : gdk_get_default_root_window()));
    guint base = 0;
    if (gdk_keymap_translate_keyboard_state(km, ev->hardware_keycode, GdkModifierType(0), ev->group, &base, nullptr,
                                            nullptr, nullptr) &&
        base)
        kv = base;
    c.vk = vkForKeyval(kv);
    if (!c.vk)
        c.vk = vkForKeyval(ev->keyval); // keypad etc.
    return c;
}

} // namespace npp::ui
