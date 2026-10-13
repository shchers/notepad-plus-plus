#include "ui/PluginManager.h"

#include "core/PluginFolders.h"
#include "platform/Paths.h"
#include "ui/EditorView.h"

// The plugin SDK: upstream's plugin headers on the port's <windows.h> shim.
#include <windows.h>
#include "PluginInterface.h"
#include "resource.h"

#include <dlfcn.h>
#include <strings.h>

#include <algorithm>
#include <exception>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace npp::ui {

static_assert(sizeof(wchar_t) == sizeof(gunichar), "the plugin interface expects UTF-32 wchar_t");

std::string fromWide(const wchar_t* s)
{
    if (!s)
        return {};
    gchar* u = g_ucs4_to_utf8(reinterpret_cast<const gunichar*>(s), -1, nullptr, nullptr, nullptr);
    std::string out = u ? u : "";
    g_free(u);
    return out;
}

std::wstring toWide(std::string_view utf8)
{
    glong n = 0;
    gunichar* w = g_utf8_to_ucs4_fast(utf8.data(), static_cast<glong>(utf8.size()), &n);
    std::wstring out(reinterpret_cast<const wchar_t*>(w), static_cast<std::size_t>(n));
    g_free(w);
    return out;
}

namespace {

struct Registered {
    PluginManager* owner;
    PluginManager::Handle kind;
};

std::unordered_map<const void*, Registered>& registry()
{
    static std::unordered_map<const void*, Registered> r;
    return r;
}

GThread* g_mainThread = nullptr;

// SCI_GETDIRECTFUNCTION on a view handle: the editor behind it changes with
// the active document, so the "direct" call goes through the handle.
sptr_t viewDirect(sptr_t ptr, unsigned int msg, uptr_t wParam, sptr_t lParam)
{
    return PluginManager::send(reinterpret_cast<const void*>(ptr), msg, wParam, lParam);
}

sptr_t viewDirectStatus(sptr_t ptr, unsigned int msg, uptr_t wParam, sptr_t lParam, int* status)
{
    const sptr_t r = viewDirect(ptr, msg, wParam, lParam);
    if (status)
        *status = static_cast<int>(viewDirect(ptr, SCI_GETSTATUS, 0, 0));
    return r;
}

// Runs `fn` on the GTK thread and waits for it, as SendMessage from another
// thread waits for the window's thread on Windows.
template <typename Fn>
auto onMainThread(Fn fn) -> decltype(fn())
{
    using Result = decltype(fn());
    if (!g_mainThread || g_thread_self() == g_mainThread)
        return fn();
    struct Call {
        Fn* work;
        Result result{};
        bool done = false;
        GMutex mutex{};
        GCond cond{};
    } call{&fn};
    g_mutex_init(&call.mutex);
    g_cond_init(&call.cond);
    g_main_context_invoke(nullptr, +[](gpointer p) -> gboolean {
        auto* c = static_cast<Call*>(p);
        Result r = (*c->work)();
        g_mutex_lock(&c->mutex);
        c->result = r;
        c->done = true;
        g_cond_signal(&c->cond);
        g_mutex_unlock(&c->mutex);
        return G_SOURCE_REMOVE;
    }, &call);
    g_mutex_lock(&call.mutex);
    while (!call.done)
        g_cond_wait(&call.cond, &call.mutex);
    g_mutex_unlock(&call.mutex);
    g_mutex_clear(&call.mutex);
    g_cond_clear(&call.cond);
    return call.result;
}

int runMessageBox(const void* hWnd, const std::string& text, const std::string& caption, unsigned type)
{
    if (PluginManager::messageBoxHook)
        return PluginManager::messageBoxHook(text, caption, type);
    GtkMessageType mt = GTK_MESSAGE_OTHER;
    switch (type & 0xF0) {
    case MB_ICONHAND: mt = GTK_MESSAGE_ERROR; break;
    case MB_ICONQUESTION: mt = GTK_MESSAGE_QUESTION; break;
    case MB_ICONEXCLAMATION: mt = GTK_MESSAGE_WARNING; break;
    case MB_ICONASTERISK: mt = GTK_MESSAGE_INFO; break;
    default: break;
    }
    GtkWidget* dlg = gtk_message_dialog_new(PluginManager::windowFor(hWnd), GTK_DIALOG_MODAL, mt, GTK_BUTTONS_NONE, "%s", text.c_str());
    gtk_window_set_title(GTK_WINDOW(dlg), caption.c_str());
    std::vector<std::pair<const char*, int>> buttons;
    switch (type & 0x0F) {
    case MB_OKCANCEL: buttons = {{"_OK", IDOK}, {"_Cancel", IDCANCEL}}; break;
    case MB_ABORTRETRYIGNORE: buttons = {{"_Abort", IDABORT}, {"_Retry", IDRETRY}, {"_Ignore", IDIGNORE}}; break;
    case MB_YESNOCANCEL: buttons = {{"_Yes", IDYES}, {"_No", IDNO}, {"_Cancel", IDCANCEL}}; break;
    case MB_YESNO: buttons = {{"_Yes", IDYES}, {"_No", IDNO}}; break;
    case MB_RETRYCANCEL: buttons = {{"_Retry", IDRETRY}, {"_Cancel", IDCANCEL}}; break;
    default: buttons = {{"_OK", IDOK}}; break;
    }
    for (const auto& [label, id] : buttons)
        gtk_dialog_add_button(GTK_DIALOG(dlg), label, id);
    const std::size_t def = (type & MB_DEFBUTTON3) ? 2 : (type & MB_DEFBUTTON2) ? 1 : 0;
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), buttons[std::min(def, buttons.size() - 1)].second);
    const int r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r > 0)
        return r;
    // Closed with Escape or the window manager: Cancel when there is one, as Windows.
    for (const auto& b : buttons)
        if (b.second == IDCANCEL)
            return IDCANCEL;
    return buttons.size() == 1 ? IDOK : IDNO;
}

} // namespace

std::function<int(const std::string& text, const std::string& caption, unsigned type)> PluginManager::messageBoxHook;

// Marks the plugin Notepad++ is calling into (NPPM_GETPLUGINHOMEPATH uses it).
class PluginManager::Calling {
public:
    Calling(PluginManager& m, const Plugin* p) : m_(m), prev_(m.calling_) { m.calling_ = p; }
    ~Calling() { m_.calling_ = prev_; }
    Calling(const Calling&) = delete;
    Calling& operator=(const Calling&) = delete;

private:
    PluginManager& m_;
    const Plugin* prev_;
};

PluginManager::PluginManager(Host host)
    : host_(std::move(host)), cmdIds_{ID_PLUGINS_CMD_DYNAMIC, ID_PLUGINS_CMD_DYNAMIC_LIMIT},
      markers_{MARKER_PLUGINS, MARKER_PLUGINS_LIMIT}, indicators_{INDICATOR_PLUGINS, INDICATOR_PLUGINS_LIMIT + 1}
{
    if (!g_mainThread)
        g_mainThread = g_thread_self();
    registry()[nppHandle()] = {this, Handle::Npp};
    registry()[viewHandle(0)] = {this, Handle::MainView};
    registry()[viewHandle(1)] = {this, Handle::SubView};
}

PluginManager::~PluginManager()
{
    for (auto it = registry().begin(); it != registry().end();)
        it = it->second.owner == this ? registry().erase(it) : std::next(it);
    for (GtkWidget* w : scintillas_) {
        g_signal_handlers_disconnect_by_data(w, this);
        g_object_unref(w);
    }
}

std::vector<fs::path> PluginManager::defaultDirs()
{
    std::vector<fs::path> dirs;
    if (const char* env = g_getenv("NPP_GTK_PLUGINS_DIR")) {
        gchar** parts = g_strsplit(env, ":", -1);
        for (gchar** p = parts; *p; ++p)
            if (**p)
                dirs.emplace_back(*p);
        g_strfreev(parts);
        return dirs;
    }
    dirs.push_back(fs::path(g_get_user_data_dir()) / NPP_EXE_NAME / "plugins");
    dirs.push_back((platform::executableDir() / ".." / NPP_PLUGINS_SUBDIR).lexically_normal());
    return dirs;
}

bool PluginManager::loadOne(const fs::path& file, const fs::path& root, const NppData& data, std::string& error)
{
    void* lib = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char* e = dlerror();
        error = e ? e : "cannot be loaded";
        return false;
    }
    auto setInfo = reinterpret_cast<void (*)(NppData)>(dlsym(lib, "setInfo"));
    auto getName = reinterpret_cast<const wchar_t* (*)()>(dlsym(lib, "getName"));
    auto getFuncsArray = reinterpret_cast<FuncItem* (*)(int*)>(dlsym(lib, "getFuncsArray"));
    auto beNotified = reinterpret_cast<void (*)(SCNotification*)>(dlsym(lib, "beNotified"));
    auto messageProc = reinterpret_cast<sptr_t (*)(unsigned, uptr_t, sptr_t)>(dlsym(lib, "messageProc"));
    auto isUnicode = reinterpret_cast<BOOL (*)()>(dlsym(lib, "isUnicode"));
    const char* missing = !setInfo ? "setInfo" : !getName ? "getName" : !getFuncsArray ? "getFuncsArray"
                        : !beNotified ? "beNotified" : !messageProc ? "messageProc" : !isUnicode ? "isUnicode" : nullptr;
    if (missing) {
        error = std::string("not a Notepad++ plugin (no \"") + missing + "\" function)";
        dlclose(lib);
        return false;
    }

    auto p = std::make_unique<Plugin>();
    p->folder = file.stem().string();
    p->moduleName = file.filename().string();
    p->file = file;
    p->root = root;
    p->lib = lib;
    p->beNotified = beNotified;
    p->messageProc = messageProc;
    p->version = core::pluginVersion(file);
    try {
        Calling c(*this, p.get());
        if (!isUnicode()) {
            error = "this ANSI plugin is not compatible with Notepad++";
            return false;
        }
        setInfo(data);
        p->displayName = fromWide(getName());
        p->items = getFuncsArray(&p->nbItems);
    } catch (const std::exception& e) {
        error = std::string("exception while loading: ") + e.what();
        return false;
    } catch (...) {
        error = "exception while loading";
        return false;
    }
    if (!p->items || p->nbItems <= 0) {
        error = "missing \"FuncItems\" array, or the number of items is not set correctly";
        return false;
    }
    if (p->displayName.empty())
        p->displayName = p->folder;
    plugins_.push_back(std::move(p));
    return true;
}

std::vector<std::string> PluginManager::load(const std::vector<fs::path>& dirs)
{
    std::vector<std::string> errors;
    NppData data;
    data._nppHandle = static_cast<HWND>(nppHandle());
    data._scintillaMainHandle = static_cast<HWND>(viewHandle(0));
    data._scintillaSecondHandle = static_cast<HWND>(viewHandle(1));

    std::vector<std::string> seen;
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        std::vector<fs::path> subs;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec))
                subs.push_back(e.path());
        std::sort(subs.begin(), subs.end(), [](const fs::path& a, const fs::path& b) {
            return strcasecmp(a.filename().c_str(), b.filename().c_str()) < 0;
        });
        for (const fs::path& sub : subs) {
            const std::string name = sub.filename().string();
            const fs::path file = sub / (name + ".so");
            if (!fs::is_regular_file(file, ec))
                continue;
            if (std::any_of(seen.begin(), seen.end(), [&](const std::string& s) { return strcasecmp(s.c_str(), name.c_str()) == 0; }))
                continue; // the same plugin in a later directory
            seen.push_back(name);
            std::string err;
            if (!loadOne(file, dir, data, err))
                errors.push_back(file.string() + ": " + err);
        }
    }

    // Upstream PluginsManager::addInMenuFromPMIndex: commands are numbered
    // from ID_PLUGINS_CMD in plugin order, separators (no function) skipped,
    // and the number is written back into the plugin's FuncItem.
    for (std::size_t i = 0; i < plugins_.size(); ++i) {
        Plugin& p = *plugins_[i];
        for (int j = 0; j < p.nbItems; ++j) {
            FuncItem& it = p.items[j];
            if (!it._pFunc)
                continue;
            const int cmdId = ID_PLUGINS_CMD + static_cast<int>(commands_.size());
            if (cmdId > ID_PLUGINS_CMD_LIMIT) {
                errors.push_back(p.moduleName + ": too many plugin commands, the rest are left out");
                return errors;
            }
            it._cmdID = cmdId;
            Command c;
            c.cmdId = cmdId;
            c.plugin = i;
            c.internalId = j;
            c.name = fromWide(std::wstring(it._itemName, wcsnlen(it._itemName, menuItemSize)).c_str());
            if (it._pShKey)
                c.defaultKey = {it._pShKey->_isCtrl, it._pShKey->_isAlt, it._pShKey->_isShift, it._pShKey->_key};
            c.initChecked = it._init2Check;
            commands_.push_back(std::move(c));
        }
    }
    return errors;
}

const PluginManager::Command* PluginManager::command(int cmdId) const
{
    const int i = cmdId - ID_PLUGINS_CMD;
    if (i < 0 || i >= static_cast<int>(commands_.size()))
        return nullptr;
    return &commands_[static_cast<std::size_t>(i)];
}

const PluginManager::Plugin* PluginManager::findModule(std::string_view moduleName) const
{
    for (const auto& p : plugins_)
        if (p->moduleName.size() == moduleName.size() && strncasecmp(p->moduleName.c_str(), moduleName.data(), moduleName.size()) == 0)
            return p.get();
    return nullptr;
}

FuncItem* PluginManager::item(const Command& c) const
{
    return &plugins_[c.plugin]->items[c.internalId];
}

void PluginManager::run(int cmdId)
{
    const Command* c = command(cmdId);
    if (!c)
        return;
    const Plugin* p = pluginOf(*c);
    PFUNCPLUGINCMD fn = item(*c)->_pFunc;
    if (!fn)
        return;
    try {
        Calling guard(*this, p);
        fn();
    } catch (const std::exception& e) {
        g_warning("Plugin %s: exception in \"%s\": %s", p->moduleName.c_str(), c->name.c_str(), e.what());
    } catch (...) {
        g_warning("Plugin %s: exception in \"%s\"", p->moduleName.c_str(), c->name.c_str());
    }
}

void PluginManager::notify(const SCNotification& n)
{
    if (shutDown_)
        return;
    shutDown_ = n.nmhdr.code == NPPN_SHUTDOWN;
    for (std::size_t i = 0; i < plugins_.size(); ++i)
        notify(*plugins_[i], n);
}

void PluginManager::notify(const Plugin& p, const SCNotification& n)
{
    SCNotification copy = n; // a plugin must not change what the next one gets
    try {
        Calling guard(*this, &p);
        p.beNotified(&copy);
    } catch (const std::exception& e) {
        g_warning("Plugin %s: exception in beNotified(%u): %s", p.moduleName.c_str(), n.nmhdr.code, e.what());
    } catch (...) {
        g_warning("Plugin %s: exception in beNotified(%u)", p.moduleName.c_str(), n.nmhdr.code);
    }
}

void PluginManager::notify(unsigned code, const void* hwndFrom, uptr_t idFrom)
{
    SCNotification n{};
    n.nmhdr.code = code;
    n.nmhdr.hwndFrom = const_cast<void*>(hwndFrom);
    n.nmhdr.idFrom = idFrom;
    notify(n);
}

bool PluginManager::relay(std::string_view moduleName, unsigned msg, uptr_t wParam, sptr_t lParam)
{
    const Plugin* p = findModule(moduleName);
    if (!p)
        return false;
    try {
        Calling guard(*this, p);
        p->messageProc(msg, wParam, lParam);
    } catch (...) {
        g_warning("Plugin %s: exception in messageProc(%u)", p->moduleName.c_str(), msg);
    }
    return true;
}

GtkWidget* PluginManager::createScintilla()
{
    GtkWidget* w = scintilla_new();
    g_object_ref_sink(w);
    scintillas_.push_back(w);
    registry()[w] = {this, Handle::Scintilla};
    g_signal_connect(w, "destroy", G_CALLBACK(+[](GtkWidget* self, gpointer) { registry().erase(self); }), this);
    return w;
}

sptr_t PluginManager::sendToView(int view, const void* handle, unsigned msg, uptr_t wParam, sptr_t lParam)
{
    switch (msg) {
    case SCI_GETDIRECTFUNCTION:
        return reinterpret_cast<sptr_t>(&viewDirect);
    case SCI_GETDIRECTSTATUSFUNCTION:
        return reinterpret_cast<sptr_t>(&viewDirectStatus);
    case SCI_GETDIRECTPOINTER:
        return reinterpret_cast<sptr_t>(handle);
    case SCI_SETDOCPOINTER:
        return 0; // each tab of the port has its own editor: documents are not swapped in a view
    default:
        break;
    }
    EditorView* v = host_.viewEditor ? host_.viewEditor(view) : nullptr;
    return v ? v->execute(msg, wParam, lParam) : 0;
}

sptr_t PluginManager::send(const void* handle, unsigned msg, uptr_t wParam, sptr_t lParam)
{
    auto it = registry().find(handle);
    if (it == registry().end())
        return 0;
    PluginManager& m = *it->second.owner;
    switch (it->second.kind) {
    case Handle::Npp:
        return m.host_.nppMessage ? m.host_.nppMessage(msg, wParam, lParam) : 0;
    case Handle::MainView:
        return m.sendToView(0, handle, msg, wParam, lParam);
    case Handle::SubView:
        return m.sendToView(1, handle, msg, wParam, lParam);
    case Handle::Scintilla:
        return scintilla_send_message(SCINTILLA(const_cast<void*>(handle)), msg, wParam, lParam);
    }
    return 0;
}

bool PluginManager::isHandle(const void* handle)
{
    return registry().count(handle) != 0;
}

GtkWindow* PluginManager::windowFor(const void* handle)
{
    auto it = registry().find(handle);
    if (it == registry().end())
        it = registry().begin();
    if (it == registry().end() || !it->second.owner->host_.window)
        return nullptr;
    return it->second.owner->host_.window();
}

} // namespace npp::ui

// ---- What plugins link against (declared in the SDK's windows.h) -----------

using npp::ui::PluginManager;

extern "C" LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    return npp::ui::onMainThread([=] { return PluginManager::send(hWnd, Msg, wParam, lParam); });
}

extern "C" BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (!npp::ui::onMainThread([=] { return PluginManager::isHandle(hWnd); }))
        return FALSE;
    struct Posted {
        HWND h;
        UINT m;
        WPARAM w;
        LPARAM l;
    };
    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
        const auto* x = static_cast<Posted*>(p);
        PluginManager::send(x->h, x->m, x->w, x->l); // 0 if the handle went away meanwhile
        return G_SOURCE_REMOVE;
    }, new Posted{hWnd, Msg, wParam, lParam}, +[](gpointer p) { delete static_cast<Posted*>(p); });
    return TRUE;
}

extern "C" int MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType)
{
    const std::string text = npp::ui::fromWide(lpText);
    const std::string caption = lpCaption ? npp::ui::fromWide(lpCaption) : std::string("Error");
    return npp::ui::onMainThread([&] { return npp::ui::runMessageBox(hWnd, text, caption, uType); });
}
