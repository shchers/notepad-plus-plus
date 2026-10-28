# Writing plugins for the Notepad++ GTK port

The port loads plugins written against **Notepad++'s own plugin interface**:
the same six exported functions, the same `NppData`, `FuncItem` and
`SCNotification`, the same `NPPM_*` and `SCI_*` messages sent with
`SendMessage`, and the same notifications. A Windows plugin's source keeps
its message handling and its logic. What changes is the binary (a `.so`
built on Linux) and the user interface: there is no Win32 API, so dialogs and
panels are GTK 3 widgets.

1. [Quick start](#1-quick-start)
2. [The SDK](#2-the-sdk)
3. [Where plugins are loaded from](#3-where-plugins-are-loaded-from)
4. [Porting a Windows plugin](#4-porting-a-windows-plugin)
5. [Messages](#5-messages)
6. [Notifications](#6-notifications)
7. [Menus, shortcuts, toolbar, panels](#7-menus-shortcuts-toolbar-panels)
8. [Flatpak and snap](#8-flatpak-and-snap)

---

## 1. Quick start

The sample plugin `linux/sdk/examples/HelloGtk` (installed with the SDK under
`/usr/share/doc/notepad-plus-plus-gtk-dev/examples/`, or
`.../notepad-plus-plus-gtk/examples/` from the CPack package) has a menu with a
default shortcut, a toolbar button and a docked panel that follows the
current document:

```bash
sudo apt install notepad-plus-plus-gtk-dev     # or: cmake --install of the port
cp -r /usr/share/doc/notepad-plus-plus-gtk-dev/examples/HelloGtk .
cmake -S HelloGtk -B HelloGtk/build
cmake --build HelloGtk/build
cmake --install HelloGtk/build    # -> ~/.local/share/notepad-plus-plus-gtk/plugins/HelloGtk/HelloGtk.so
```

Restart Notepad++: the commands are under **Plugins ▸ Hello GTK**.

A larger example ships with the editor: the Remote Files plugin
(`linux/plugins/NppRemote`, [REMOTE_FILES.md](REMOTE_FILES.md)), with a
docked panel, dialogs, a worker thread that calls back into the GTK thread,
and uploads on `NPPN_FILESAVED`.

The whole plugin side of a "hello" command is what it would be on Windows:

```cpp
#include <windows.h>          // the SDK's, not Win32's
#include "PluginInterface.h"  // upstream's, unchanged
#include "NppGtk.h"           // port extensions (optional)

NppData npp;

void hello()
{
    int view = 0;
    ::SendMessage(npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&view));
    HWND sci = view == 0 ? npp._scintillaMainHandle : npp._scintillaSecondHandle;
    ::SendMessage(sci, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("Hello!"));
}

extern "C" __declspec(dllexport) void setInfo(NppData d) { npp = d; }
// getName, getFuncsArray, beNotified, messageProc, isUnicode as usual
```

## 2. The SDK

Installed by `notepad-plus-plus-gtk-dev` (or `cmake --install` of the port)
into `<prefix>/include/notepad-plus-plus-gtk/`:

| Header | From | What it is |
|---|---|---|
| `windows.h`, `tchar.h` | port | The Win32 types the plugin headers use (`HWND`, `BOOL`, `UINT`, `WPARAM`, `LPARAM`, `LRESULT`, `RECT`, `COLORREF`, ...), `MAX_PATH`, `WM_USER`, `MAKELONG`/`LOWORD`/`HIWORD`, `MB_*`/`ID*`, `lstrcpy`-style and `wcscpy_s` helpers, and three functions: `SendMessageW`, `PostMessageW`, `MessageBoxW` (`SendMessage` etc. are macros for them, as in a Unicode Windows build). It is not the Win32 API. |
| `PluginInterface.h`, `Notepad_plus_msgs.h` | upstream, unchanged | Entry points, `NppData`, `FuncItem`, `ShortcutKey`, all `NPPM_*`/`NPPN_*` |
| `Docking.h`, `dockingResource.h` | upstream, unchanged | `DockedWidgetData` (`tTbData`), `DWS_*`, `DMN_*` |
| `menuCmdID.h` | upstream, unchanged | `IDM_*` for `NPPM_MENUCOMMAND` |
| `Scintilla.h`, `Sci_Position.h`, `ScintillaWidget.h` | Scintilla | `SCI_*`, `SCN_*`, `SCNotification`; the GTK widget API for Scintilla controls you create |
| `NppGtk.h` | port | What differs from Windows, the `NPPM_GTK*` messages, and helpers (`NppGtk_handle`, `NppGtk_widget`, `NppGtk_icon`) |

`pkg-config notepad-plus-plus-gtk-plugin` gives the include flags (and pulls
in GTK 3); `pkg-config --variable=plugindir notepad-plus-plus-gtk-plugin` is
the system-wide plugin directory.

Build the plugin as a shared object with hidden visibility (the entry points
are exported by `__declspec(dllexport)`, which the SDK maps to
`visibility("default")`), named after its folder: `HelloGtk/HelloGtk.so`.
`SendMessageW` and friends are left undefined in the `.so`; the Notepad++
executable provides them at load time.

### Strings

`wchar_t` is 32 bits (UTF-32) on Linux. The interface keeps `wchar_t`, so
`L"..."` literals, `wcslen`, `wcscpy_s` and `std::wstring` keep working and
the buffer-size semantics of every `NPPM_GET*` message are unchanged (sizes
count `wchar_t`, not bytes). Two things to watch:

- In `swprintf`/`wprintf` formats, `%s` means a `char*` string on Linux; use
  `%ls` for `wchar_t*`.
- Scintilla text is UTF-8 (`char*`), as on Windows. Convert with
  `g_ucs4_to_utf8` / `g_utf8_to_ucs4` (GLib), not with `WideCharToMultiByte`.

## 3. Where plugins are loaded from

At startup (unless `-noPlugin`), each directory below is searched for
`<Name>/<Name>.so`, folders in name order:

1. `$XDG_DATA_HOME/notepad-plus-plus-gtk/plugins` (usually
   `~/.local/share/notepad-plus-plus-gtk/plugins`; **Plugins ▸ Open Plugins
   Folder** opens it)
2. `<prefix>/<libdir>/notepad-plus-plus-gtk/plugins` (for example
   `/usr/lib/x86_64-linux-gnu/notepad-plus-plus-gtk/plugins`)

A name found in the first directory is not loaded again from the second, so
a user can update a plugin without root. `NPP_GTK_PLUGINS_DIR` (a
`:`-separated list) replaces both; set it empty to load none.

**Plugins ▸ Plugins Admin** lists the plugins of both directories with their
version, location and status ("Loaded", or why not), in upstream's
Installed and Deactivated tabs. In the user's directory it installs a plugin
from a `<Name>.so`, or a `.zip` holding `<Name>.so` at its top or in a
`<Name>/` folder (how upstream's plugins are published), replacing one of the
same name; it removes plugins, and deactivates them by moving them to
`plugins/disabled/`, as upstream does. Settings ▸ Import ▸ Import plugin(s)
installs files the same way. Changes apply at the next start. Plugins in the
system directory belong to the package manager and are shown read-only.
Upstream's Available and Updates tabs, which download Windows DLLs from its
plugin list, are not there.

The version shown is what the plugin declares with `NPPGTK_PLUGIN_VERSION`
(`NppGtk.h`; Windows reads a DLL's version resource instead). It is a tagged
string in the `.so`, read without loading the plugin, so it also shows for
plugins installed or deactivated since the start. A `.so` copied straight
into the plugins directory, without its `<Name>/` folder, is listed with a
note: Notepad++ does not load it there.

```cpp
NPPGTK_PLUGIN_VERSION("1.2.0")   // at file scope, once
```

Loading follows upstream: `isUnicode` must return `TRUE`, then `setInfo`,
`getName`, `getFuncsArray`. A file that lacks one of the six functions, or
cannot be loaded, is reported on stderr and skipped. Plugins stay loaded
until the process exits (they may have registered GTypes).

Plugin settings go to `NPPM_GETPLUGINSCONFIGDIR`
(`~/.config/notepad-plus-plus-gtk/plugins/config`, created at startup).
`NPPM_GETPLUGINHOMEPATH` returns the directory the calling plugin was loaded
from (on Windows there is only one).

## 4. Porting a Windows plugin

What carries over unchanged:

- The entry points, `FuncItem` arrays (separators are items without a
  function), `_init2Check`, default `ShortcutKey`s.
- Everything done with `SendMessage` to `_nppHandle` or the two Scintilla
  handles, including `SCI_GETDIRECTFUNCTION`/`SCI_GETDIRECTPOINTER`. The
  direct pointer of a view handle follows the document shown in that view,
  as on Windows.
- `beNotified` with `NPPN_*` and the editors' `SCN_*`; `messageProc` with
  `NPPM_MSGTOPLUGIN`; `MessageBox`.
- Buffer IDs, command IDs (`ID_PLUGINS_CMD` + n, written back into
  `FuncItem::_cmdID`), `NPPM_ALLOCATE*` ranges.
- `SendMessage` from a worker thread: it runs on the GTK thread and waits,
  as Windows does for a window owned by another thread. `PostMessage` is
  queued to the GTK main loop.

What needs rewriting:

- **Dialogs and panels**: Win32 dialogs (`CreateDialogParam`, `.rc`
  resources, `StaticDialog`/`DockingDlgInterface`) become GTK widgets. For a
  modal dialog, use `NPPM_GTKGETMAINWINDOW` as the transient parent. For a
  docked panel, see §7.
- **Other Win32 calls** (`GetWindowText`, `SetWindowPos`, `CheckMenuItem` on
  `NPPM_GETMENUHANDLE`, registry, `ShellExecute`, ...): use GLib/GTK or the
  matching `NPPM_*` message (`NPPM_SETMENUITEMCHECK` instead of
  `CheckMenuItem`).
- **The module name** in `DockedWidgetData::pszModuleName`, `shortcuts.xml`
  and `NPPM_MSGTOPLUGIN` is `"<Name>.so"` instead of `"<Name>.dll"`.
- **Toolbar icons** are icon names or image paths, not `HBITMAP`/`HICON` (§7).

A plugin that builds for both platforms keeps its message code shared and
puts the user interface behind `#ifdef _WIN32`.

## 5. Messages

Every `NPPM_*` of `Notepad_plus_msgs.h` is answered; the return values
follow upstream's `NppBigSwitch.cpp` (where its comments and its code differ,
the code). These behave differently on Linux:

| Message | On the GTK port |
|---|---|
| `NPPM_GETMENUHANDLE` | 0: GTK menus are not `HMENU`s. Use `NPPM_SETMENUITEMCHECK`. |
| `NPPM_MODELESSDIALOG` | Returns `hDlg`; GTK dialogs handle their own keys. |
| `NPPM_CREATESCINTILLAHANDLE` | Returns a Scintilla `GtkWidget` (a `ScintillaObject`) cast to `HWND`: pack it in your UI (`NppGtk_widget`), send it messages, connect to its `"sci-notify"` signal. Notepad++ keeps a reference until exit. |
| `NPPM_ADDTOOLBARICON_FORDARKMODE`, `NPPM_ADDTOOLBARICON_DEPRECATED` | Icon fields carry `const char*` (see §7). |
| `NPPM_DMMREGASDCKDLG` and the other `NPPM_DMM*` | `hClient` is a `GtkWidget` (see §7). |
| `NPPM_GETWINDOWSVERSION` | `WV_UNKNOWN`. |
| `NPPM_GETCURRENTNATIVELANGENCODING` | 65001 (UTF-8). |
| `NPPM_GETBUFFERENCODING`, `NPPM_SETBUFFERENCODING` | Upstream's `UniMode` numbers; legacy code pages report 0 ("ANSI"), and "ANSI" is set as UTF-8, the Linux system code page. |
| `NPPM_ISDARKMODEENABLED`, `NPPM_GETDARKMODECOLORS` | Dark mode is a dark editor theme (the GTK chrome follows it). The colours: background and text from the theme, the others fixed. |
| `NPPM_DARKMODESUBCLASSANDTHEME` | Returns the flags: GTK already themes your widgets. |
| `NPPM_GETTOOLBARICONSETCHOICE` | 4 ("Standard icons: small"). |
| `NPPM_ENCODESCI`, `NPPM_DECODESCI`, `NPPM_GETENABLETHEMETEXTUREFUNC_DEPRECATED`, `NPPM_GETSETTINGSONCLOUDPATH` | 0. |
| `NPPM_GET/SETEXTERNALLEXERAUTOINDENTMODE` | `FALSE`: no external lexers. |
| `NPPM_SETEDITORBORDEREDGE`, `NPPM_DOCLISTDISABLEEXTCOLUMN`, `NPPM_DOCLISTDISABLEPATHCOLUMN`, `NPPM_DISABLEAUTOUPDATE` | `TRUE`, no effect. |
| `NPPM_SETLINENUMBERWIDTHMODE` | Remembered and returned by `NPPM_GETLINENUMBERWIDTHMODE`; the margin is always dynamic. |
| `NPPM_GETTABCOLORID` | -1: tabs have no colours yet. |
| `SCI_SETDOCPOINTER` on a view handle | Ignored: each tab has its own editor, documents are not swapped in a view. |
| `WM_COMMAND` to `_nppHandle` | Runs the command in `LOWORD(wParam)`, like `NPPM_MENUCOMMAND`. |

Port-only messages (`NppGtk.h`, `NPPM_GTKBASE` + n):

| Message | Returns |
|---|---|
| `NPPM_GTKGETMAINWINDOW(0, 0)` | The main `GtkWindow*`. |
| `NPPM_GTKGETSCINTILLAWIDGET(0, HWND)` | The Scintilla widget behind a handle; for a view handle, the editor shown now (do not keep it). |
| `NPPM_GTKGETPORTVERSION(0, 0)` | 2 (`NPPGTK_INTERFACE_VERSION`); 0 on Windows. |
| `NPPM_GTKGETSAVETARGET(bufferID, wchar_t*)` | Since 2, in `NPPN_FILEBEFORESAVE`: the file being written (a Save As's new name); its length, 0 when that buffer is not being saved. |
| `NPPM_GTKWRITEBUFFER(bufferID, const wchar_t*)` | Since 2: writes the document's bytes as a save would (encoding, BOM) to another file; the document keeps its name and state. |
| `NPPM_GTKCANCELSAVE(bufferID, 0)` | Since 2, in `NPPN_FILEBEFORESAVE`: stops the save. Nothing is written, the tab stays modified, no message (the plugin gives one), no `NPPN_FILESAVED`. For plugins that store the file elsewhere first (Remote Files uploads it). |

## 6. Notifications

Sent as upstream sends them, with the same `hwndFrom`/`idFrom`:
`NPPN_TBMODIFICATION` and `NPPN_READY` at startup (after the session and the
command-line files are open), `NPPN_CMDLINEPLUGINMSG` (`-pluginMessage=`),
`NPPN_FILEBEFORELOAD`/`NPPN_FILELOADFAILED`, `NPPN_FILEBEFOREOPEN`/
`NPPN_FILEOPENED`, `NPPN_SNAPSHOTDIRTYFILELOADED`, `NPPN_FILEBEFORESAVE`
(the text can still be changed) / `NPPN_FILESAVED`, `NPPN_FILEBEFORECLOSE`/
`NPPN_FILECLOSED`, `NPPN_FILEBEFORERENAME`/`NPPN_FILERENAMECANCEL`/
`NPPN_FILERENAMED`, `NPPN_FILEBEFOREDELETE`/`NPPN_FILEDELETEFAILED`/
`NPPN_FILEDELETED`, `NPPN_BUFFERACTIVATED`, `NPPN_LANGCHANGED`,
`NPPN_READONLYCHANGED`, `NPPN_DOCORDERCHANGED` (the Move Tab commands),
`NPPN_WORDSTYLESUPDATED`,
`NPPN_SHORTCUTREMAPPED`, `NPPN_DARKMODECHANGED`, `NPPN_NATIVELANGCHANGED`,
`NPPN_GLOBALMODIFIED` (Replace All), `NPPN_BEFORESHUTDOWN`/
`NPPN_CANCELSHUTDOWN`, and `NPPN_SHUTDOWN` (once, while your widgets still
exist). Not sent: `NPPN_EXTERNALLEXERBUFFER` (no external lexers) and
`NPPN_TOOLBARICONSETCHANGED`.

The editors' `SCN_*` notifications reach `beNotified` from the document
shown in each view, with `hwndFrom` = `_scintillaMainHandle` or
`_scintillaSecondHandle`. `SCN_MODIFIED` comes with upstream's default flags
(`SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_PERFORMED_UNDO |
SC_PERFORMED_REDO | SC_MOD_CHANGEINDICATOR`); add more with
`NPPM_ADDSCNMODIFIEDFLAGS`.

## 7. Menus, shortcuts, toolbar, panels

**Menu.** Each plugin gets a submenu of **Plugins** named by `getName()`;
`&` in item names marks the mnemonic. Items with `_init2Check` show a check
mark; `NPPM_SETMENUITEMCHECK` sets it (and adds one to an item that had
none).

**Shortcuts.** A `_pShKey` is the default. The user changes it in
Settings ▸ Shortcut Mapper ▸ Plugin commands, which writes upstream's
`<PluginCommands><PluginCommand moduleName="X.so" internalID="<index>" .../>`
to `shortcuts.xml` (`NPPN_SHORTCUTREMAPPED` follows).
`NPPM_GETSHORTCUTBYCMDID` and `NPPM_REMOVESHORTCUTBYCMDID` work as upstream.

**Toolbar.** During `NPPN_TBMODIFICATION`, send
`NPPM_ADDTOOLBARICON_FORDARKMODE` with the icons as strings:

```cpp
toolbarIconsWithDarkMode icons{nullptr,
                               NppGtk_icon("edit-find"),             // or "/full/path/icon.png"
                               NppGtk_icon("edit-find-symbolic")};   // for dark themes; optional
::SendMessage(npp._nppHandle, NPPM_ADDTOOLBARICON_FORDARKMODE, funcItems[0]._cmdID, reinterpret_cast<LPARAM>(&icons));
```

`hToolbarIcon` (or else `hToolbarBmp`) is an icon theme name or the path of
an image file (scaled to 16 px); `hToolbarIconDarkMode` is used with dark
themes. The strings are copied.

**Docked panels.** A panel is any `GtkWidget`:

```cpp
GtkWidget* panel = gtk_label_new("...");
DockedWidgetData d{};
d.hClient = NppGtk_handle(panel);
d.pszName = L"My Panel";                 // caption; kept by pointer, as upstream
d.dlgID = 2;                             // index of the FuncItem that shows the panel
d.uMask = DWS_DF_CONT_RIGHT;             // or LEFT/TOP/BOTTOM, DWS_DF_FLOATING; DWS_ADDINFO + pszAddInfo
d.pszModuleName = L"MyPlugin.so";
::SendMessage(npp._nppHandle, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&d));
::SendMessage(npp._nppHandle, NPPM_DMMSHOW, 0, reinterpret_cast<LPARAM>(panel));
```

Notepad++ takes a reference on the widget and destroys it with the window.
`NPPM_DMMHIDE`, `NPPM_DMMUPDATEDISPINFO` (re-reads `pszName`/`pszAddInfo`
through the pointers you registered), `NPPM_DMMVIEWOTHERTAB` and
`NPPM_DMMGETPLUGINHWNDBYNAME` work as upstream. The user can move the panel
to any side or float it; its place is saved, and a panel open at exit is
opened again at the next start by running the command at index `dlgID`, as
upstream does.

When the user closes the panel, Windows sends `DMN_CLOSE` to the panel's
window. A GTK widget has no message queue, so the port calls the owning
plugin's `beNotified` with `nmhdr.code = DMN_CLOSE`, `hwndFrom = hClient`
and `idFrom = dlgID`; uncheck the menu item there.

## 8. Flatpak and snap

The Flatpak and snap builds look in their own data directory:
`~/.var/app/<app-id>/data/notepad-plus-plus-gtk/plugins` (Flatpak) and
`~/snap/notepad-plus-plus-gtk/current/.local/share/notepad-plus-plus-gtk/plugins`
(snap). A plugin there runs inside the sandbox, against the runtime's
libraries: build it with the matching SDK (for Flatpak,
`flatpak run --command=bash org.gnome.Sdk//<version>` with the port's
headers) or keep its dependencies to GTK, GLib and the C/C++ runtime.
