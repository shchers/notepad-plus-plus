// HelloGtk — a minimal Notepad++ plugin for the GTK port, in the style of
// upstream's plugin template: three menu commands, a toolbar button and a
// dockable panel that follows the current document.
//
// Build it against the installed SDK (see CMakeLists.txt next to it):
//   cmake -S . -B build && cmake --build build && cmake --install build
// and restart Notepad++: the commands are under Plugins ▸ Hello GTK.

#include <windows.h>
#include "PluginInterface.h"
#include "NppGtk.h"

#include <gtk/gtk.h>

#include <string>

NPPGTK_PLUGIN_VERSION("1.0")

namespace {

NppData npp;
constexpr int kCommands = 4;
FuncItem funcItems[kCommands];
ShortcutKey helloKey{true, true, false, 'H'}; // Ctrl+Alt+H, changeable in the Shortcut Mapper
constexpr int kPanelCommand = 2;              // index of "Current Document Panel": the panel's dlgID

GtkWidget* panel = nullptr;
GtkWidget* panelLabel = nullptr;
DockedWidgetData panelData;
bool panelShown = false;
toolbarIconsWithDarkMode toolbarButton{nullptr, NppGtk_icon("face-smile"), NppGtk_icon("face-smile-symbolic")};

HWND currentScintilla()
{
    int which = -1;
    ::SendMessage(npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
    return which == 0 ? npp._scintillaMainHandle : npp._scintillaSecondHandle;
}

// The UTF-8 text Scintilla wants from a wide string.
std::string utf8(const wchar_t* s)
{
    gchar* u = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(s), -1, nullptr, nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

void hello()
{
    // Scintilla messages go to the editor handles, as on Windows.
    ::SendMessage(currentScintilla(), SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("Hello, Notepad++ on GTK!"));
}

void insertPath()
{
    wchar_t path[MAX_PATH] = {};
    if (::SendMessage(npp._nppHandle, NPPM_GETFULLCURRENTPATH, MAX_PATH, reinterpret_cast<LPARAM>(path)))
        ::SendMessage(currentScintilla(), SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>(utf8(path).c_str()));
}

void updatePanel()
{
    if (!panelLabel)
        return;
    wchar_t name[MAX_PATH] = {};
    ::SendMessage(npp._nppHandle, NPPM_GETFILENAME, MAX_PATH, reinterpret_cast<LPARAM>(name));
    const LRESULT lines = ::SendMessage(currentScintilla(), SCI_GETLINECOUNT, 0, 0);
    const std::string text = utf8(name) + "\n" + std::to_string(lines) + " lines";
    gtk_label_set_text(GTK_LABEL(panelLabel), text.c_str());
}

void togglePanel()
{
    if (!panel) {
        // A plugin panel is any GTK widget; Notepad++ docks it and keeps it alive.
        panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(panel), 8);
        panelLabel = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(panelLabel), 0);
        gtk_box_pack_start(GTK_BOX(panel), panelLabel, FALSE, FALSE, 0);

        panelData = DockedWidgetData{};
        panelData.hClient = NppGtk_handle(panel);
        panelData.pszName = L"Current Document";
        panelData.dlgID = kPanelCommand;
        panelData.uMask = DWS_DF_CONT_RIGHT;
        panelData.pszModuleName = L"HelloGtk.so";
        ::SendMessage(npp._nppHandle, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&panelData));
    }
    panelShown = !panelShown;
    ::SendMessage(npp._nppHandle, panelShown ? NPPM_DMMSHOW : NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(panel));
    ::SendMessage(npp._nppHandle, NPPM_SETMENUITEMCHECK, funcItems[kPanelCommand]._cmdID, panelShown);
    updatePanel();
}

void about()
{
    ::MessageBox(npp._nppHandle, L"A sample plugin for the Notepad++ GTK port.", L"Hello GTK", MB_OK | MB_ICONINFORMATION);
}

void setCommand(int i, const wchar_t* name, PFUNCPLUGINCMD fn, ShortcutKey* key = nullptr)
{
    wcscpy_s(funcItems[i]._itemName, name);
    funcItems[i]._pFunc = fn;
    funcItems[i]._pShKey = key;
}

} // namespace

extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    npp = data;
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"Hello GTK";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count)
{
    setCommand(0, L"Say &Hello", hello, &helloKey);
    setCommand(1, L"Insert Current &Path", insertPath);
    setCommand(kPanelCommand, L"Current Document &Panel", togglePanel);
    setCommand(3, L"&About", about);
    *count = kCommands;
    return funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* n)
{
    switch (n->nmhdr.code) {
    case NPPN_TBMODIFICATION:
        ::SendMessage(npp._nppHandle, NPPM_ADDTOOLBARICON_FORDARKMODE, funcItems[0]._cmdID, reinterpret_cast<LPARAM>(&toolbarButton));
        break;
    case NPPN_BUFFERACTIVATED:
    case NPPN_FILESAVED:
        updatePanel();
        break;
    case DMN_CLOSE: // the user closed the panel (port extension, see NppGtk.h)
        panelShown = false;
        ::SendMessage(npp._nppHandle, NPPM_SETMENUITEMCHECK, funcItems[kPanelCommand]._cmdID, FALSE);
        break;
    case NPPN_SHUTDOWN:
        panel = panelLabel = nullptr; // destroyed with the window
        break;
    default:
        break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM)
{
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}
