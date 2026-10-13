// Plugins — upstream MISC/PluginsManager on Linux. A plugin is a shared
// object built against the SDK in linux/sdk (upstream's PluginInterface.h
// and Notepad_plus_msgs.h with a small <windows.h> shim): the same exports
// (setInfo, getName, getFuncsArray, beNotified, messageProc, isUnicode) and
// the same messages, sent with SendMessage, which the executable exports.
//
// This class loads the plugins, numbers their commands as upstream does
// (ID_PLUGINS_CMD + n), broadcasts notifications and owns the handles a
// plugin talks to: NppData's three, and the Scintilla widgets it creates.
// What a message does is MainWindow's business (MainWindowPlugins.cpp).
#pragma once

#include "core/KeyCombo.h"

// Order matters: ScintillaWidget.h expects GTK types to be declared.
#include <gtk/gtk.h>
#include <Scintilla.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct FuncItem;
struct NppData;

namespace npp::ui {

class EditorView;

// wchar_t on Linux is UTF-32: plugin strings to and from UTF-8.
std::string fromWide(const wchar_t* s);
std::wstring toWide(std::string_view utf8);

class PluginManager {
public:
    struct Plugin {
        std::string folder;      // "<Name>"
        std::string moduleName;  // "<Name>.so": shortcuts.xml, docking, NPPM_MSGTOPLUGIN
        std::string displayName; // getName(): the Plugins submenu
        std::string version;     // NPPGTK_PLUGIN_VERSION, "" if none
        std::filesystem::path file;
        std::filesystem::path root; // the plugins directory it was found in
        void* lib = nullptr;
        FuncItem* items = nullptr;
        int nbItems = 0;
        void (*beNotified)(SCNotification*) = nullptr;
        sptr_t (*messageProc)(unsigned, uptr_t, sptr_t) = nullptr;
    };
    struct Command {
        int cmdId = 0;
        std::size_t plugin = 0; // index in plugins()
        int internalId = 0;     // index in the plugin's FuncItem array
        std::string name;       // UTF-8
        core::KeyCombo defaultKey;
        bool initChecked = false;
    };
    enum class Handle { Npp, MainView, SubView, Scintilla };

    // What the handles reach.
    struct Host {
        std::function<sptr_t(unsigned msg, uptr_t wParam, sptr_t lParam)> nppMessage;
        std::function<EditorView*(int view)> viewEditor; // the view's current editor, may be null
        std::function<GtkWindow*()> window;
    };

    explicit PluginManager(Host host);
    ~PluginManager(); // the libraries stay loaded: they may have registered GTypes
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void* nppHandle() { return &handles_[0]; }
    void* viewHandle(int view) { return &handles_[view == 0 ? 1 : 2]; }

    // NPP_GTK_PLUGINS_DIR (':'-separated) when set, else the user's
    // $XDG_DATA_HOME/<exe>/plugins, then <prefix>/<libdir>/<exe>/plugins.
    static std::vector<std::filesystem::path> defaultDirs();
    // Loads <dir>/<Name>/<Name>.so from each directory, in name order; a
    // name found twice is taken from the first directory. Calls setInfo,
    // getName and getFuncsArray, then numbers the commands. Returns the
    // problems (one line each); plugins with problems are skipped.
    std::vector<std::string> load(const std::vector<std::filesystem::path>& dirs);

    const std::vector<std::unique_ptr<Plugin>>& plugins() const { return plugins_; }
    const std::vector<Command>& commands() const { return commands_; }
    const Command* command(int cmdId) const;
    const Plugin* findModule(std::string_view moduleName) const; // case-insensitive, as upstream
    const Plugin* pluginOf(const Command& c) const { return plugins_[c.plugin].get(); }
    FuncItem* item(const Command& c) const;

    void run(int cmdId);
    // Each plugin gets its own copy. Nothing is sent after NPPN_SHUTDOWN.
    void notify(const SCNotification& n);
    void notify(unsigned code, const void* hwndFrom, uptr_t idFrom);
    // To one plugin only (DMN_CLOSE for its panel).
    void notify(const Plugin& p, const SCNotification& n);
    // NPPM_MSGTOPLUGIN: messageProc of the plugin with that module name.
    bool relay(std::string_view moduleName, unsigned msg, uptr_t wParam, sptr_t lParam);
    // The plugin Notepad++ is calling into right now (setInfo, a command,
    // a notification), or null.
    const Plugin* calling() const { return calling_; }

    // Upstream IDAllocator ranges. -1 when exhausted.
    int allocateCmdIds(int n) { return cmdIds_.allocate(n); }
    int allocateMarkers(int n) { return markers_.allocate(n); }
    int allocateIndicators(int n) { return indicators_.allocate(n); }

    // NPPM_CREATESCINTILLAHANDLE: a Scintilla widget the plugin packs where
    // it likes; also a handle for SendMessage.
    GtkWidget* createScintilla();

    // SendMessage on a handle; 0 for handles nobody registered.
    static sptr_t send(const void* handle, unsigned msg, uptr_t wParam, sptr_t lParam);
    static bool isHandle(const void* handle);
    // The window to put a plugin's message box on.
    static GtkWindow* windowFor(const void* handle);
    // Tests: answers MessageBox instead of a dialog (UTF-8 text, caption, MB_* type).
    static std::function<int(const std::string& text, const std::string& caption, unsigned type)> messageBoxHook;

private:
    struct IdAllocator {
        int next;
        int max;
        int allocate(int n)
        {
            if (n <= 0 || next + n > max)
                return -1;
            const int start = next;
            next += n;
            return start;
        }
    };
    class Calling;
    sptr_t sendToView(int view, const void* handle, unsigned msg, uptr_t wParam, sptr_t lParam);
    bool loadOne(const std::filesystem::path& file, const std::filesystem::path& root, const NppData& data, std::string& error);

    Host host_;
    std::array<char, 3> handles_{};
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<Command> commands_;
    std::vector<GtkWidget*> scintillas_;
    const Plugin* calling_ = nullptr;
    bool shutDown_ = false;
    IdAllocator cmdIds_;
    IdAllocator markers_;
    IdAllocator indicators_;
};

} // namespace npp::ui
