// Main window — GTK counterpart of upstream Notepad_plus / Notepad_plus_Window
// (menus, two views with tabs, status bar). See PORTING.md for how the
// remaining upstream modules map onto this structure.
//
// Documents (core::Buffer) are independent of views. Each tab owns an
// EditorView; a buffer shown in both views ("clone") has two tabs whose
// Scintilla widgets share one Scintilla document.
//
// MainWindow.cpp      construction, menus, commands, signal handlers
// MainWindowDocs.cpp  tabs and buffers, file I/O, sessions, snapshots,
//                     outside-change detection
#pragma once

#include "core/Buffer.h"
#include "core/FileManager.h"
#include "core/LanguageCatalog.h"
#include "core/RecentFiles.h"
#include "core/Session.h"
#include "core/StyleCatalog.h"
#include "platform/FileMonitor.h"
#include "platform/Settings.h"
#include "ui/EditorView.h"
#include "ui/FindBar.h"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace npp::ui {

// Upstream command line switches that apply to files being opened.
struct OpenOptions {
    std::optional<long> line;      // -n<line>, 1-based
    std::optional<long> column;    // -c<column>, 1-based
    const core::Language* language = nullptr; // -l<lang>
    bool readOnly = false;         // -ro
};

class MainWindow {
public:
    static constexpr int kMainView = 0;
    static constexpr int kSubView = 1;

    // Answers to questions the window asks (save changes? reload?).
    enum class Answer { Yes, No, Cancel };
    // Replaces the modal dialogs, for tests. `cancellable` adds a Cancel choice.
    using PromptHandler = std::function<Answer(const std::string& primary, const std::string& secondary, bool cancellable)>;

    // `useSession` false: -nosession, or a second instance under -multiInst.
    // Disables session restore/save and snapshots, as upstream.
    MainWindow(GtkApplication* app, core::LanguageCatalog& langs, bool useSession = true);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    GtkWindow* window() const { return GTK_WINDOW(window_); }
    void present();

    void newDocument();
    // Opens files (or switches to them if already open). Returns how many opened.
    int openFiles(const std::vector<std::filesystem::path>& files, const OpenOptions& opts = {});

    // ---- Sessions (upstream session.xml format) ----------------------------
    static std::filesystem::path lastSessionPath();
    // Startup: restores <config>/session.xml when sessions are enabled.
    void restoreLastSession();
    // Opens the session's documents next to the ones already open.
    bool loadSession(const std::filesystem::path& file, std::string* error = nullptr);
    bool saveSession(const std::filesystem::path& file, std::string* error = nullptr);
    // Snapshot paths are included when snapshots are on and a buffer is dirty.
    core::Session currentSession() const;
    // Periodic backup tick: writes snapshots of changed unsaved documents,
    // then the session. Runs on a timer when snapshots are enabled.
    void snapshotNow();

    // ---- Views --------------------------------------------------------------
    void moveToOtherView();   // upstream "Move to Other View"
    void cloneToOtherView();  // upstream "Clone to Other View"
    void switchToOtherView(); // F8

    // Handles file monitor events that are waiting (also run on focus-in).
    void checkExternalChanges();
    // The exit protocol behind closing the window: snapshot mode saves the
    // session without asking; otherwise asks about unsaved documents. Returns
    // false if the user cancelled.
    bool requestQuit();

    // ---- Introspection and test hooks -------------------------------------
    void setPromptHandler(PromptHandler h) { prompt_ = std::move(h); }
    std::size_t documentCount() const;                // distinct buffers
    std::size_t tabCount(int view) const;
    int activeViewIndex() const { return activeView_; }
    EditorView* activeView() const;
    core::Buffer* activeBuffer() const;
    bool isModified(const core::Buffer& b) const;
    std::string activeTitle() const;
    const core::RecentFiles& recentFiles() const { return recent_; }
    platform::Settings& settings() { return settings_; }

private:
    struct Tab {
        std::unique_ptr<EditorView> view;
        std::shared_ptr<core::Buffer> buf;
        int viewIdx = kMainView;
        GtkWidget* label = nullptr;    // event box holding text + close button
        GtkWidget* labelText = nullptr;
    };

    // Construction helpers (MainWindow.cpp)
    void buildActions();
    void buildMenus(GtkWidget* vbox);
    void buildToolbar(GtkWidget* vbox);
    void buildStatusBar(GtkWidget* vbox);
    GtkWidget* buildNotebook(int viewIdx);
    void loadStyles();
    std::vector<std::string> availableThemes() const;
    void importConfigOnFirstRun(bool settingsExisted);

    // Tabs and buffers (MainWindowDocs.cpp)
    Tab* addTab(std::shared_ptr<core::Buffer> buf, int viewIdx, const Tab* cloneOf = nullptr);
    void attachLabel(Tab& t);
    void removeTab(Tab& t);        // no questions; releases the buffer with its last tab
    void releaseBuffer(core::Buffer& b);
    Tab* activeTab() const;
    Tab* currentTabOf(int viewIdx) const;
    Tab* tabForPage(GtkWidget* page) const;
    Tab* tabOf(const core::Buffer& b, int viewIdx) const;
    std::vector<Tab*> tabsOf(const core::Buffer& b) const;
    std::vector<Tab*> tabsInOrder(int viewIdx) const;
    std::vector<core::Buffer*> buffers() const;
    core::Buffer* findByPath(const std::filesystem::path& p) const;
    void activateTab(Tab& t, bool focus = true);
    void updateViewVisibility();
    std::string nextUntitledName() const;

    // Puts loaded text into the tab's document; `p` empty for untitled.
    void applyLoaded(Tab& t, const std::filesystem::path& p, core::LoadedFile&& lf);
    bool reloadBuffer(core::Buffer& b, std::string* error);
    bool save(core::Buffer& b, bool saveAs);
    bool confirmSave(core::Buffer& b);       // asks to save; false = cancel
    bool closeTab(Tab& t, bool keepOne = true); // false if the user cancelled
    bool closeAll();
    void setLanguage(core::Buffer& b, const core::Language& lang);
    void setReadOnly(core::Buffer& b, bool ro);
    void markForcedDirty(core::Buffer& b, bool dirty);

    void watch(core::Buffer& b);
    void rewatchAll();
    void onFileEvent(core::Buffer& b);
    void scheduleChangeCheck();
    void restartSnapshotTimer();
    Tab* openSessionFile(const core::SessionFile& f, int viewIdx);
    bool sessionOn() const { return useSession_ && settings_.rememberSession; }
    // Upstream only offers snapshots together with "Remember current session".
    bool snapshotsOn() const { return sessionOn() && settings_.snapshot; }

    // UI refresh (MainWindow.cpp)
    void refreshTab(Tab& t);
    void refreshBufferTabs(core::Buffer& b);
    void refreshTitle();
    void refreshStatus();
    void refreshAll();
    void refreshRecentMenu();
    void syncActionStates();
    void applyViewSettings(EditorView& v);
    void showError(const std::string& primary, const std::string& secondary);
    Answer ask(const std::string& primary, const std::string& secondary, bool cancellable);

    // Commands (MainWindow.cpp)
    void cmdOpen();
    void cmdLoadSession();
    void cmdSaveSession();
    void cmdImportConfig();
    void cmdGotoLine();
    void cmdAbout();
    void cmdSetTheme(const std::string& theme);
    void cmdSetEncoding(const std::string& id);
    void cmdSetEol(const std::string& id);
    void cmdRestoreRecent();
    void cmdOpenAllRecent();
    void openRecentFile(const std::string& file);
    void sciCommand(unsigned msg);
    void showTabMenu(Tab& t, const GdkEvent* ev);

    // Signal trampolines
    static void onAction(GSimpleAction* a, GVariant* param, gpointer self);
    static void onStatefulAction(GSimpleAction* a, GVariant* value, gpointer self);
    static void onOpenRecent(GSimpleAction* a, GVariant* param, gpointer self);
    static void onSwitchPage(GtkNotebook* nb, GtkWidget* page, guint, gpointer self);
    static gboolean onDeleteEvent(GtkWidget*, GdkEvent*, gpointer self);
    static gboolean onKeyPress(GtkWidget* w, GdkEventKey* ev, gpointer self);
    static void onDragData(GtkWidget*, GdkDragContext* ctx, gint, gint, GtkSelectionData* data, guint, guint time, gpointer self);
    static gboolean onConfigure(GtkWidget*, GdkEventConfigure* ev, gpointer self);
    static gboolean onWindowState(GtkWidget*, GdkEventWindowState* ev, gpointer self);
    static void onActiveChanged(GObject*, GParamSpec*, gpointer self);
    void openUriList(std::string_view uris);

    GtkApplication* app_;
    core::LanguageCatalog& langs_;
    core::StyleCatalog styles_;
    platform::Settings settings_;
    core::RecentFiles recent_;
    const bool useSession_;

    GtkWidget* window_ = nullptr;
    GtkWidget* paned_ = nullptr;
    GtkWidget* notebooks_[2] = {nullptr, nullptr};
    std::unique_ptr<FindBar> findBar_;
    GtkWidget* stLang_ = nullptr;
    GtkWidget* stStats_ = nullptr;
    GtkWidget* stPos_ = nullptr;
    GtkWidget* stEol_ = nullptr;
    GtkWidget* stEnc_ = nullptr;
    GtkWidget* stIns_ = nullptr;
    GMenu* recentSection_ = nullptr;
    GtkWidget* tabMenu_ = nullptr;

    std::unordered_map<std::string, std::function<void()>> commands_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    std::unordered_map<const core::Buffer*, std::unique_ptr<platform::FileMonitor>> monitors_;
    int activeView_ = kMainView;
    std::filesystem::path lastDir_;
    PromptHandler prompt_;
    bool syncingActions_ = false;
    bool closing_ = false;         // tearing down: buffers keep their snapshots
    bool checkingChanges_ = false;
    std::optional<core::Session> lastWrittenSession_;
    guint refreshIdle_ = 0;
    guint changeCheckIdle_ = 0;
    guint snapshotTimer_ = 0;
};

} // namespace npp::ui
