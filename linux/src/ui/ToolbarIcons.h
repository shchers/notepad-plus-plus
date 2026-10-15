// Upstream's toolbar (Notepad_plus.cpp toolBarIcons, WinControls/ToolBar)
// with its own icons, PowerEditor/src/icons, staged as <data>/icons:
//
//   {light,dark}/toolbar/{regular,filled}/<stem>_off.ico  Fluent UI sets
//   {light,dark}/toolbar/{regular,filled}/<stem>_dis.ico  disabled, for some
//   standard/toolbar/<name>.bmp                            Standard icons
//
// Each .ico holds the icon drawn at 16 to 96 pixels; the size the toolbar
// shows is taken as drawn rather than scaled from another.
#pragma once

#include <gtk/gtk.h>

#include <filesystem>

namespace npp::ui {

struct ToolbarButton {
    int id;            // command; 0 = separator
    const char* stem;  // Fluent icon files
    const char* bmp;   // Standard icon file
    const char* theme; // desktop icon theme name, when the files are missing
    const char* tip;   // English tooltip, when the translation has none
};

// Upstream's buttons in upstream's order.
extern const ToolbarButton kToolbarButtons[];
extern const int kToolbarButtonCount;

// Preferences > Toolbar (upstream toolBarStatusType): 0 Fluent UI small,
// 1 Fluent UI large, 2 Filled Fluent UI small, 3 Filled Fluent UI large,
// 4 Standard icons small.
inline bool toolbarLarge(int set) { return set == 1 || set == 3; }
inline int toolbarIconPixels(int set) { return toolbarLarge(set) ? 32 : 16; }

// The icon file of a button for an icon set, light or dark; disabled
// returns an empty path when the button has no disabled icon.
std::filesystem::path toolbarIconFile(const std::filesystem::path& iconsDir, const ToolbarButton& b, int set, bool dark, bool disabled);

// A toolbar icon `px` pixels wide: from a .ico, the entry of that size (or
// the nearest larger one, scaled down); from a .bmp, upstream's grey
// background (192,192,192, LR_LOADMAP3DCOLORS) made transparent. nullptr if
// it cannot be read.
GdkPixbuf* loadToolbarIcon(const std::filesystem::path& file, int px);

} // namespace npp::ui
