// Notepad++ GTK port — plugin SDK: what is different from, or in addition
// to, the Windows plugin interface. It includes PluginInterface.h and
// Docking.h.
//
// Windows handles and GTK widgets
//  - NppData::_nppHandle, _scintillaMainHandle and _scintillaSecondHandle are
//    handles for SendMessage only. The two Scintilla handles always address
//    the document shown in that view, as on Windows.
//  - NPPM_CREATESCINTILLAHANDLE returns a Scintilla GtkWidget (a
//    ScintillaObject) cast to HWND: pack it into your own GTK container
//    (NppGtk_widget()), and SendMessage works on it too. Its notifications
//    come from its "sci-notify" signal.
//  - DockedWidgetData::hClient is your panel's GtkWidget cast to HWND
//    (NppGtk_handle()). Notepad++ takes a reference on it.
//  - Toolbar icons (NPPM_ADDTOOLBARICON_FORDARKMODE): toolbarIconsWithDarkMode
//    fields hold a `const char*` cast to the handle type — an icon theme name
//    ("edit-find-symbolic") or the full path of an image file. Without a dark
//    icon, dark themes use the light one. NppGtk_icon() does the cast.
//  - When the user closes a docked panel, Windows sends DMN_CLOSE to the
//    panel's window (WM_NOTIFY). A GTK widget has no message queue, so the
//    port calls the owning plugin's beNotified with nmhdr.code = DMN_CLOSE,
//    nmhdr.hwndFrom = hClient and nmhdr.idFrom = dlgID. Uncheck your menu
//    item there (NPPM_SETMENUITEMCHECK).
//  - SendMessage from a worker thread is carried out on the GTK thread while
//    the caller waits, as Windows does for a window of another thread.
//
// Plugin files
//  - A plugin is <Name>/<Name>.so in a plugins directory (as <Name>\<Name>.dll
//    on Windows), so the module name used in shortcuts.xml,
//    DockedWidgetData::pszModuleName and NPPM_MSGTOPLUGIN is "<Name>.so".
#pragma once

#include "PluginInterface.h"
#include "Docking.h"
#include "dockingResource.h" // DMN_CLOSE

// Messages only the GTK port understands (NPPM_GTKBASE + n). They are not
// sent by Windows plugins, so a plugin built for both can use them under
// #ifndef _WIN32.
#define NPPM_GTKBASE (NPPMSG + 1000)

	#define NPPM_GTKGETMAINWINDOW (NPPM_GTKBASE + 1)
	// GtkWindow* NPPM_GTKGETMAINWINDOW(0, 0)
	// The Notepad++ main window, e.g. as the transient parent of a plugin's dialogs.

	#define NPPM_GTKGETSCINTILLAWIDGET (NPPM_GTKBASE + 2)
	// GtkWidget* NPPM_GTKGETSCINTILLAWIDGET(0, HWND hScintilla)
	// The Scintilla widget behind a Scintilla handle. For _scintillaMainHandle and
	// _scintillaSecondHandle it is the editor currently shown in that view: it
	// changes when another document is activated, so do not keep it.

	#define NPPM_GTKGETPORTVERSION (NPPM_GTKBASE + 3)
	// int NPPM_GTKGETPORTVERSION(0, 0)
	// Version of this extension interface (1). 0 from any other host.

#define NPPGTK_INTERFACE_VERSION 1

// Optional: the version Plugins Admin shows (Windows reads the DLL's version
// resource, which a .so does not have). Use once, at file scope:
//   NPPGTK_PLUGIN_VERSION("1.2.0")
// It is a tagged string in the file, so the version is known without loading
// the plugin (a plugin installed or deactivated since the start).
#define NPPGTK_PLUGIN_VERSION(text) \
    extern "C" __attribute__((visibility("default"), used)) const char nppGtkPluginVersion[] = "NppGtkPluginVersion:" text;

#ifdef __cplusplus
// GtkWidget* <-> HWND, for hClient, NPPM_CREATESCINTILLAHANDLE and friends.
template <typename Widget>
inline HWND NppGtk_handle(Widget* w) { return reinterpret_cast<HWND>(w); }
template <typename Widget = struct _GtkWidget>
inline Widget* NppGtk_widget(HWND h) { return reinterpret_cast<Widget*>(h); }
// An icon name or image path in a toolbarIconsWithDarkMode field.
template <typename Handle = HICON>
inline Handle NppGtk_icon(const char* nameOrPath) { return reinterpret_cast<Handle>(const_cast<char*>(nameOrPath)); }
#endif
