// A plugin for the plugin tests (plugins_test.cpp), built like a third-party
// plugin: only against the SDK (linux/sdk) and GTK, never the port's code.
// It records what it sees in a key/value log that the test reads through
// testPlugin_get(); the commands exercise the messages a plugin typically sends.

#include <windows.h>
#include "PluginInterface.h"
#include "Docking.h"
#include "menuCmdID.h"
#include "NppGtk.h"

#include <gtk/gtk.h>

#include <atomic>
#include <map>
#include <string>
#include <thread>

NPPGTK_PLUGIN_VERSION("2.5.1")

namespace {

NppData npp;
constexpr int kItems = 7;
FuncItem items[kItems];
ShortcutKey shortcutKey{true, true, true, 'T'}; // Ctrl+Alt+Shift+T
std::map<std::string, std::string> state;
int notifications[64] = {}; // per NPPN_* (code - NPPN_FIRST)
int mainViewScn = 0;        // SCN_* from the main view's handle
GtkWidget* panel = nullptr;
DockedWidgetData dock;
std::atomic<long> threadResult{-1};
toolbarIconsWithDarkMode icons{nullptr, NppGtk_icon("face-smile"), NppGtk_icon("face-smile-symbolic")};

LRESULT send(UINT msg, WPARAM w = 0, LPARAM l = 0)
{
    return ::SendMessage(npp._nppHandle, msg, w, l);
}

std::string narrow(const wchar_t* s)
{
    std::string out;
    for (; s && *s; ++s)
        out += *s < 0x80 ? static_cast<char>(*s) : '?';
    return out;
}

// The current editor through its handle, as plugins do.
HWND currentScintilla()
{
    int which = -1;
    send(NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
    return which == 0 ? npp._scintillaMainHandle : npp._scintillaSecondHandle;
}

void sayHello()
{
    HWND sci = currentScintilla();
    // Through the "direct" function, as performance-minded plugins do.
    auto fn = reinterpret_cast<SciFnDirect>(::SendMessage(sci, SCI_GETDIRECTFUNCTION, 0, 0));
    auto ptr = static_cast<sptr_t>(::SendMessage(sci, SCI_GETDIRECTPOINTER, 0, 0));
    const char text[] = "hello from plugin";
    fn(ptr, SCI_APPENDTEXT, sizeof text - 1, reinterpret_cast<sptr_t>(text));
}

void toggleCheck()
{
    static bool on = true; // _init2Check
    on = !on;
    send(NPPM_SETMENUITEMCHECK, items[2]._cmdID, on);
}

void showPanel()
{
    if (!panel) {
        panel = gtk_label_new("Test panel");
        dock = DockedWidgetData{};
        dock.hClient = NppGtk_handle(panel);
        dock.pszName = L"Test Panel";
        dock.dlgID = 3;
        dock.uMask = DWS_DF_CONT_RIGHT | DWS_ADDINFO;
        dock.pszAddInfo = L"info";
        dock.pszModuleName = L"TestPlugin.so";
        send(NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&dock));
    }
    send(NPPM_DMMSHOW, 0, reinterpret_cast<LPARAM>(panel));
}

void shortcutCommand()
{
    ShortcutKey sk;
    state["shortcut"] = send(NPPM_GETSHORTCUTBYCMDID, items[4]._cmdID, reinterpret_cast<LPARAM>(&sk))
                            ? std::to_string(sk._isCtrl) + std::to_string(sk._isAlt) + std::to_string(sk._isShift) + "+" + std::to_string(sk._key)
                            : "none";
    send(NPPM_SETMENUITEMCHECK, items[4]._cmdID, TRUE); // an item without _init2Check gets a check mark
}

void askUser()
{
    state["messagebox"] = std::to_string(::MessageBox(npp._nppHandle, L"Continue?", L"Test Plugin", MB_YESNO | MB_ICONQUESTION));
}

void fromThread()
{
    std::thread([] { threadResult = static_cast<long>(send(NPPM_GETNBOPENFILES, 0, ALL_OPEN_FILES)); }).detach();
}

void onReady()
{
    const LRESULT v = send(NPPM_GETNPPVERSION, TRUE);
    state["version"] = std::to_string(HIWORD(v)) + "." + std::to_string(LOWORD(v));
    state["gtkVersion"] = std::to_string(send(NPPM_GTKGETPORTVERSION));
    state["mainWindow"] = send(NPPM_GTKGETMAINWINDOW) ? "yes" : "no";

    const auto buf = static_cast<UINT_PTR>(send(NPPM_GETCURRENTBUFFERID));
    const LRESULT len = send(NPPM_GETFULLPATHFROMBUFFERID, buf, 0);
    std::wstring path(static_cast<size_t>(len), L'\0');
    send(NPPM_GETFULLPATHFROMBUFFERID, buf, reinterpret_cast<LPARAM>(path.data()));
    state["path"] = narrow(path.c_str());
    state["openFiles"] = std::to_string(send(NPPM_GETNBOPENFILES, 0, ALL_OPEN_FILES));
    state["posOfBuffer"] = std::to_string(send(NPPM_GETPOSFROMBUFFERID, buf, MAIN_VIEW));

    wchar_t dir[MAX_PATH] = {};
    const LRESULT need = send(NPPM_GETPLUGINSCONFIGDIR, 0, 0);
    state["configDirCopied"] = std::to_string(send(NPPM_GETPLUGINSCONFIGDIR, MAX_PATH, reinterpret_cast<LPARAM>(dir)));
    state["configDir"] = narrow(dir);
    state["configDirLen"] = std::to_string(need) + "/" + std::to_string(wcslen(dir));
    state["tooSmall"] = std::to_string(send(NPPM_GETPLUGINSCONFIGDIR, 3, reinterpret_cast<LPARAM>(dir)));
    wchar_t home[MAX_PATH] = {};
    send(NPPM_GETPLUGINHOMEPATH, MAX_PATH, reinterpret_cast<LPARAM>(home));
    state["home"] = narrow(home);

    wchar_t name[64] = {};
    send(NPPM_GETLANGUAGENAME, L_CPP, reinterpret_cast<LPARAM>(name));
    state["langName"] = narrow(name);
    wchar_t file[MAX_PATH] = {};
    state["fileNameOk"] = std::to_string(send(NPPM_GETFILENAME, MAX_PATH, reinterpret_cast<LPARAM>(file)));
    state["fileName"] = narrow(file);

    int start = -1;
    send(NPPM_ALLOCATECMDID, 5, reinterpret_cast<LPARAM>(&start));
    state["cmdIds"] = std::to_string(start);
    send(NPPM_ALLOCATEMARKER, 2, reinterpret_cast<LPARAM>(&start));
    state["markers"] = std::to_string(start);
    send(NPPM_ALLOCATEINDICATOR, 1, reinterpret_cast<LPARAM>(&start));
    state["indicator"] = std::to_string(start);
    const LRESULT tooMany = send(NPPM_ALLOCATEMARKER, 100, reinterpret_cast<LPARAM>(&start));
    state["tooManyMarkers"] = std::to_string(tooMany) + "," + std::to_string(start);

    // A message to itself through Notepad++.
    CommunicationInfo ci{4242, L"TestPlugin.so", nullptr};
    state["msgToPlugin"] = std::to_string(send(NPPM_MSGTOPLUGIN, reinterpret_cast<WPARAM>(L"TestPlugin.so"), reinterpret_cast<LPARAM>(&ci)));
    state["msgToNobody"] = std::to_string(send(NPPM_MSGTOPLUGIN, reinterpret_cast<WPARAM>(L"Nobody.so"), reinterpret_cast<LPARAM>(&ci)));

    // Queued, to the main view: done once the main loop runs.
    ::PostMessage(npp._scintillaMainHandle, SCI_SETEXTRAASCENT, 3, 0);
    state["postToNowhere"] = std::to_string(::PostMessage(reinterpret_cast<HWND>(&state), WM_COMMAND, 0, 0));
    send(NPPM_ADDSCNMODIFIEDFLAGS, 0, SC_MOD_BEFOREINSERT);
}

} // namespace

extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    npp = data;
    state.clear();
    for (int& n : notifications)
        n = 0;
    mainViewScn = 0;
    panel = nullptr; // a new window: the old panel went with the old one
    threadResult = -1;
    state["setInfo"] = "yes";
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"Test Plugin";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* n)
{
    const struct {
        const wchar_t* name;
        PFUNCPLUGINCMD fn;
        bool check;
        ShortcutKey* key;
    } defs[kItems] = {
        {L"&Say Hello", sayHello, false, nullptr}, {L"", nullptr, false, nullptr}, {L"Toggle Check", toggleCheck, true, nullptr},
        {L"Show Panel", showPanel, false, nullptr}, {L"Shortcut Command", shortcutCommand, false, &shortcutKey},
        {L"Ask", askUser, false, nullptr},          {L"From a Thread", fromThread, false, nullptr}};
    for (int i = 0; i < kItems; ++i) {
        items[i] = FuncItem{};
        wcscpy_s(items[i]._itemName, defs[i].name);
        items[i]._pFunc = defs[i].fn;
        items[i]._init2Check = defs[i].check;
        items[i]._pShKey = defs[i].key;
    }
    *n = kItems;
    return items;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* n)
{
    const unsigned code = n->nmhdr.code;
    if (n->nmhdr.hwndFrom == npp._scintillaMainHandle) {
        ++mainViewScn;
        if (code == SCN_MODIFIED && (n->modificationType & SC_MOD_BEFOREINSERT))
            state["beforeInsert"] = "yes";
        return;
    }
    if (code == DMN_CLOSE) { // the port's way to tell a plugin its panel was closed
        state["dmnClose"] = std::to_string(n->nmhdr.idFrom);
        return;
    }
    if (code > NPPN_FIRST && code < NPPN_FIRST + 64)
        ++notifications[code - NPPN_FIRST];
    switch (code) {
    case NPPN_TBMODIFICATION:
        state["cmdIdsAtTb"] = std::to_string(items[0]._cmdID) + "," + std::to_string(items[2]._cmdID) + "," + std::to_string(items[1]._cmdID);
        send(NPPM_ADDTOOLBARICON_FORDARKMODE, items[0]._cmdID, reinterpret_cast<LPARAM>(&icons));
        break;
    case NPPN_READY:
        onReady();
        break;
    case NPPN_FILEBEFORESAVE:
        // A plugin may still change the text before it is written.
        ::SendMessage(currentScintilla(), SCI_APPENDTEXT, 1, reinterpret_cast<LPARAM>("!"));
        break;
    case NPPN_FILEOPENED:
    case NPPN_FILESAVED:
    case NPPN_FILECLOSED:
    case NPPN_BUFFERACTIVATED:
    case NPPN_LANGCHANGED: {
        const LRESULT len = send(NPPM_GETFULLPATHFROMBUFFERID, n->nmhdr.idFrom, 0);
        std::wstring p(static_cast<size_t>(len > 0 ? len : 0), L'\0');
        if (len > 0)
            send(NPPM_GETFULLPATHFROMBUFFERID, n->nmhdr.idFrom, reinterpret_cast<LPARAM>(p.data()));
        state["last" + std::to_string(code - NPPN_FIRST)] = narrow(p.c_str());
        break;
    }
    case NPPN_READONLYCHANGED:
        state["readOnly"] = std::to_string(n->nmhdr.idFrom);
        break;
    case NPPN_CMDLINEPLUGINMSG:
        state["cmdline"] = narrow(reinterpret_cast<const wchar_t*>(n->nmhdr.idFrom));
        break;
    case NPPN_SHORTCUTREMAPPED: {
        const auto* sk = static_cast<const ShortcutKey*>(n->nmhdr.hwndFrom);
        state["remapped"] = std::to_string(n->nmhdr.idFrom) + ":" + std::to_string(sk->_key);
        break;
    }
    default:
        break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT msg, WPARAM, LPARAM l)
{
    if (msg == NPPM_MSGTOPLUGIN)
        state["received"] = std::to_string(reinterpret_cast<CommunicationInfo*>(l)->internalMsg);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}

// For the test only: what the plugin saw.
extern "C" __declspec(dllexport) const char* testPlugin_get(const char* key)
{
    static std::string value;
    const std::string k = key;
    if (k.rfind("nppn:", 0) == 0)
        value = std::to_string(notifications[std::stoi(k.substr(5)) - NPPN_FIRST]);
    else if (k == "scn")
        value = std::to_string(mainViewScn);
    else if (k == "thread")
        value = std::to_string(threadResult.load());
    else
        value = state.count(k) ? state[k] : "";
    return value.c_str();
}
