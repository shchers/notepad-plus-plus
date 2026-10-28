// NppRemote — remote files over FTP, SFTP and a shell link, in the spirit of NppFTP:
// browse a server in a docked panel, open its files in Notepad++ and send
// them back on save. Bundled with the GTK port; built only on the plugin
// SDK, GTK and POSIX (sockets, a pseudo-terminal for OpenSSH's sftp and ssh).

#include <windows.h>
#include "PluginInterface.h"
#include "NppGtk.h"

#include "Panel.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <memory>
#include <string>

NPPGTK_PLUGIN_VERSION("1.0")

namespace {

NppData npp;
constexpr int kPanelCommand = 0;
constexpr int kCommands = 6;
FuncItem funcItems[kCommands];
toolbarIconsWithDarkMode toolbarButton{nullptr, NppGtk_icon("folder-remote"), NppGtk_icon("folder-remote-symbolic")};
std::unique_ptr<nppremote::Panel> panel;

std::string narrow(const wchar_t* s)
{
    gchar* u = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(s), -1, nullptr, nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

nppremote::Panel& thePanel()
{
    if (!panel) {
        // The length first: the buffer must be larger than the path.
        const LRESULT len = ::SendMessage(npp._nppHandle, NPPM_GETPLUGINSCONFIGDIR, 0, 0);
        std::wstring dir(static_cast<std::size_t>(len > 0 ? len : 0) + 1, L'\0');
        if (len <= 0 || !::SendMessage(npp._nppHandle, NPPM_GETPLUGINSCONFIGDIR, dir.size(), reinterpret_cast<LPARAM>(dir.data())))
            dir.assign(1, L'\0');
        std::string configDir = narrow(dir.c_str());
        if (configDir.empty())
            configDir = std::string(g_get_user_config_dir()) + "/notepad-plus-plus-gtk/plugins/config";
        g_mkdir_with_parents(configDir.c_str(), 0700);
        // Downloaded files: <cache>/<profile>/<remote path>.
        const std::string cache = std::string(g_get_user_cache_dir()) + "/notepad-plus-plus-gtk/NppRemote";
        g_mkdir_with_parents(cache.c_str(), 0700);
        panel = std::make_unique<nppremote::Panel>(npp, configDir + "/NppRemote.ini", cache, kPanelCommand);
        panel->shownChanged = [](bool shown) {
            ::SendMessage(npp._nppHandle, NPPM_SETMENUITEMCHECK, funcItems[kPanelCommand]._cmdID, shown);
        };
    }
    return *panel;
}

void togglePanel()
{
    thePanel().setShown(!thePanel().shown());
}

void editProfiles()
{
    thePanel().editProfiles();
}

void uploadCurrent()
{
    thePanel().uploadCurrent();
}

void disconnect()
{
    thePanel().disconnect();
}

void about()
{
    ::MessageBox(npp._nppHandle,
                 L"Remote Files 1.0\n\n"
                 L"Edit files on FTP and SFTP servers, in the spirit of NppFTP.\n\n"
                 L"Double-click a file to open it; saving it uploads it again. "
                 L"Downloaded files are kept in ~/.cache/notepad-plus-plus-gtk/NppRemote.\n\n"
                 L"FTP is built in. SFTP uses the system's OpenSSH client (sftp), and the shell link, "
                 L"as in Midnight Commander, its ssh with plain shell commands on the server (for hosts without SFTP); "
                 L"~/.ssh/config, keys and ssh-agent apply. FTPS is not supported.",
                 L"Remote Files", MB_OK | MB_ICONINFORMATION);
}

void setCommand(int i, const wchar_t* name, PFUNCPLUGINCMD fn)
{
    wcscpy_s(funcItems[i]._itemName, name);
    funcItems[i]._pFunc = fn;
    funcItems[i]._pShKey = nullptr;
}

} // namespace

extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    npp = data;
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"Remote Files (FTP/SFTP)";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count)
{
    setCommand(kPanelCommand, L"Show &Remote Files Panel", togglePanel);
    setCommand(1, L"Edit &Profiles…", editProfiles);
    setCommand(2, L"&Upload Current Document", uploadCurrent);
    setCommand(3, L"&Disconnect", disconnect);
    setCommand(4, L"", nullptr); // separator
    setCommand(5, L"&About", about);
    *count = kCommands;
    return funcItems;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* n)
{
    switch (n->nmhdr.code) {
    case NPPN_TBMODIFICATION:
        ::SendMessage(npp._nppHandle, NPPM_ADDTOOLBARICON_FORDARKMODE, funcItems[kPanelCommand]._cmdID, reinterpret_cast<LPARAM>(&toolbarButton));
        break;
    case NPPN_FILEBEFORESAVE:
        // Upload first, and stop the save when that fails (port extension).
        if (::SendMessage(npp._nppHandle, NPPM_GTKGETPORTVERSION, 0, 0) >= 2)
            thePanel().beforeSave(n->nmhdr.idFrom);
        break;
    case NPPN_FILESAVED: {
        // Also a cached file reopened from the last session, before the
        // panel was shown: it connects and uploads.
        const UINT_PTR id = n->nmhdr.idFrom;
        const LRESULT len = ::SendMessage(npp._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, id, 0);
        if (len <= 0)
            break;
        std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
        ::SendMessage(npp._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, id, reinterpret_cast<LPARAM>(w.data()));
        thePanel().fileSaved(narrow(w.c_str()));
        break;
    }
    case DMN_CLOSE: // the user closed the panel (port extension, see NppGtk.h)
        if (panel) {
            panel->closedByUser();
            ::SendMessage(npp._nppHandle, NPPM_SETMENUITEMCHECK, funcItems[kPanelCommand]._cmdID, FALSE);
        }
        break;
    case NPPN_SHUTDOWN:
        if (panel)
            panel->shutdown();
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
