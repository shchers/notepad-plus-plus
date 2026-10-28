// Main window — GTK counterpart of upstream Notepad_plus / Notepad_plus_Window
// (menus, two views with tabs, status bar). See PORTING.md for how the
// remaining upstream modules map onto this structure.
//
// Documents (core::Buffer) are independent of views. Each tab owns an
// EditorView; a buffer shown in both views ("clone") has two tabs whose
// Scintilla widgets share one Scintilla document.
//
// MainWindow.cpp          construction, menus, refresh, signal handlers
// MainWindowCommands.cpp  menu commands by upstream ID (NppCommands.cpp)
// MainWindowDocs.cpp      tabs and buffers, file I/O, sessions, snapshots,
//                         outside-change detection
// MainWindowPlugins.cpp   plugins: the messages they send (NppBigSwitch.cpp),
//                         their menu, keys, toolbar buttons and panels
#pragma once

#include "core/Buffer.h"
#include "core/FileManager.h"
#include "core/LanguageCatalog.h"
#include "core/PluginFolders.h"
#include "core/RecentFiles.h"
#include "core/Session.h"
#include "core/Shortcuts.h"
#include "core/StyleCatalog.h"
#include "core/Translator.h"
#include "platform/FileMonitor.h"
#include "platform/Settings.h"
#include "core/FunctionParser.h"
#include "ui/CharPanel.h"
#include "ui/ClipboardHistoryPanel.h"
#include "ui/TerminalPanel.h"
#include "ui/ToolbarIcons.h"
#include "ui/Commands.h"
#include "ui/DockManager.h"
#include "ui/DocListPanel.h"
#include "ui/DocumentMap.h"
#include "ui/FolderPanel.h"
#include "ui/FunctionListPanel.h"
#include "ui/ProjectPanel.h"
#include "ui/EditorView.h"
#include "ui/FindBar.h"
#include "ui/FindDialog.h"
#include "ui/PluginManager.h"
#include "ui/SearchResults.h"

#include <gtk/gtk.h>

#include <array>
#include <filesystem>
#include <functional>
#include <map>
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
    const std::vector<core::UserLanguage>& userLanguages() const { return udls_; }
    const core::UserLanguage* userLanguage(const std::string& name) const;
    // Edit > Insert > Date Time (customized): upstream's Windows date picture.
    static std::string formatCustomDate(const std::string& windowsFormat);
    // The tab bar of a view (tests).
    GtkNotebook* notebook(int view) const { return GTK_NOTEBOOK(notebooks_[view]); }
    // Whether a point (root window coordinates) is on the tab strip of `nb`
    // but on none of its tabs.
    static bool onEmptyTabStrip(GtkNotebook* nb, double xRoot, double yRoot);
    // Runs a menu command by upstream ID (menuCmdID.h), as WM_COMMAND does.
    bool runCommand(int id) { return cmds_.run(id); }
    bool hasCommand(int id) const { return cmds_.has(id); }
    bool isEnabled(int id) const { return cmds_.isEnabled(id); }
    bool commandChecked(int id) const;
    const core::Translator& translator() const { return tr_; }
    // Switches the UI language (nativeLang file name, "" = English).
    void setLocalization(const std::string& fileName);
    // Applies the settings to every view and the window (after Preferences).
    void applySettings();
    // File > Print (with the dialog) / Print Now; `pdf` exports instead (tests).
    bool print(bool showDialog, const std::filesystem::path& pdf = {}, std::string* error = nullptr, int* pages = nullptr);
    // Re-reads shortcuts.xml and applies its keys.
    void reloadShortcuts();
    // Writes shortcuts.xml from the current keys, macros and Run commands.
    bool saveShortcuts(std::string* error = nullptr);

    // ---- Macros and Run (MainWindowMacros.cpp) -------------------------------
    void startRecording();
    void stopRecording();
    bool isRecording() const { return recording_; }
    const core::Macro& recordedMacro() const { return macro_; }
    // Plays a macro `times` times as one undo step; times < 0 = until the end of the file.
    void playMacro(const core::Macro& m, int times = 1);
    const std::vector<core::SavedMacro>& savedMacros() const { return shortcuts_.macros; }
    const std::vector<core::UserCommand>& userCommands() const { return shortcuts_.userCommands; }
    void addSavedMacro(const core::SavedMacro& m);
    void addUserCommand(const core::UserCommand& c);
    // Upstream Run command variables: $(FULL_CURRENT_PATH), $(CURRENT_WORD) ...
    std::string expandRunVariables(const std::string& cmd) const;
    bool runUserCommand(const std::string& cmd, std::string* error = nullptr);
    FindDialog& findDialog() { return *findDlg_; }
    SearchResults& searchResults() { return *results_; }
    FindBar& incrementalSearch() { return *findBar_; }

    // ---- Docking panels (MainWindowPanels.cpp) --------------------------------
    DockManager& dock() { return *dock_; }
    FunctionListPanel& functionList() { return *funcList_; }
    FolderPanel& folderPanel() { return *folder_; }
    DocumentMap& documentMap() { return *docMap_; }
    DocListPanel& documentList() { return *docList_; }
    CharPanel& charPanel() { return *charPanel_; }
    ClipboardHistoryPanel& clipboardHistory() { return *clipHistory_; }
    TerminalPanel& terminal() { return *terminal_; }
    // Whether `ev` is a shortcut of View > Terminal, the one key the
    // terminal does not get first.
    bool isTerminalKey(const GdkEventKey* ev) const;
    ProjectPanel& projectPanel(int n) { return *projects_[static_cast<std::size_t>(n - 1)]; }
    // Panel IDs as in the dock: "functionList", "folderAsWorkspace", ...
    // A plugin's panel is "plugin:<module>:<dlgID>".
    static constexpr const char* kSearchResultsPanel = "searchResults";
    void showSearchResults(bool show);
    // Opens a search result: the file (if needed), then selects the hit.
    void openResult(const ResultLocation& loc);
    // The open document showing a file (its active view), or nullptr.
    EditorView* viewForFile(const std::filesystem::path& p) const;

    // ---- Plugins (MainWindowPlugins.cpp) ---------------------------------------
    // Startup, unless -noPlugin: loads the plugins (PluginManager::defaultDirs()
    // by default), adds their commands, menu and keys, then sends
    // NPPN_TBMODIFICATION so they can add toolbar buttons.
    void loadPlugins();
    void loadPlugins(const std::vector<std::filesystem::path>& dirs);
    // -pluginMessage=...: NPPN_CMDLINEPLUGINMSG.
    void sendPluginCommandLine(const std::string& message);
    // NPPM_GETCURRENTCMDLINE answers this.
    void setCommandLine(std::string cmdLine) { cmdLine_ = std::move(cmdLine); }
    // Preferences > Multi-Instance "Open session in a new instance": this
    // session file is written again when the window closes.
    void setExitSessionFile(std::filesystem::path f) { exitSessionFile_ = std::move(f); }
    // Startup done: NPPN_READY, then the plugin panels that were open at the
    // last exit are opened again (through their plugin's command, as upstream).
    void pluginsReady();
    PluginManager* plugins() { return plugins_.get(); }
    const std::vector<std::string>& pluginErrors() const { return pluginErrors_; }
    // What SendMessage(nppData._nppHandle, msg, wParam, lParam) does.
    sptr_t pluginMessage(unsigned msg, uptr_t wParam, sptr_t lParam);
    // Plugin commands for the Shortcut Mapper, with their current keys.
    struct PluginCommandInfo {
        int cmdId = 0;
        std::string name;
        std::string plugin;  // menu name of the plugin
        std::string module;  // "<Name>.so"
        int internalId = 0;
        core::KeyCombo key;
    };
    std::vector<PluginCommandInfo> pluginCommandList() const;
    // NPPN_SHORTCUTREMAPPED after the Shortcut Mapper changed a plugin command's key.
    void notifyShortcutRemapped(int cmdId);
    // Plugins Admin: what a plugin folder's row says ("Loaded", "Deactivated",
    // "Not loaded: <why>", ...).
    std::string pluginFolderStatus(const core::PluginFolder& f, const std::vector<core::PluginFolder>& all) const;
    // Installs plugin files (.so, .zip, folders) into the user's plugin
    // directory, asking before replacing one. Returns how many were installed.
    int installPlugins(const std::vector<std::filesystem::path>& files);

private:
    struct Tab {
        std::unique_ptr<EditorView> view;
        std::shared_ptr<core::Buffer> buf;
        int viewIdx = kMainView;
        GtkWidget* label = nullptr;    // event box holding text + close button
        GtkWidget* labelText = nullptr;
        GtkWidget* closeButton = nullptr;
    };

    // Construction helpers (MainWindow.cpp)
    void buildActions();
    void buildMenus();
    void applyShortcuts();
    void refreshLanguageMenu();
    void refreshThemeMenu();
    void buildToolbar(GtkWidget* vbox);
    void buildStatusBar(GtkWidget* vbox);
    GtkWidget* buildNotebook(int viewIdx);
    void loadStyles();
    std::filesystem::path themePath(const std::string& theme) const; // "" = stylers.xml
    void restyleAll();      // re-applies styles_ to every view
    void cmdStyleConfigurator();
    std::vector<std::string> availableThemes() const;
    void importConfigOnFirstRun(bool settingsExisted);

    // Tabs and buffers (MainWindowDocs.cpp)
    Tab* addTab(std::shared_ptr<core::Buffer> buf, int viewIdx, const Tab* cloneOf = nullptr);
    void attachLabel(Tab& t);
    void removeTab(Tab& t);        // no questions; releases the buffer with its last tab
    void releaseBuffer(core::Buffer& b);
    Tab* activeTab() const;
    Tab* currentTabOf(int viewIdx) const;
    Tab* tabAt(int viewIdx, int page) const;
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
    // `codepage` forces a Windows code page (Encoding > Character Set), -1 detects.
    bool reloadBuffer(core::Buffer& b, std::string* error, int codepage = -1);
    // `to`: save there without asking (a plugin's NPPM_SAVECURRENTFILEAS).
    bool save(core::Buffer& b, bool saveAs, const std::filesystem::path& to = {});
    bool saveCopyAs(core::Buffer& b);
    void renameBuffer(core::Buffer& b);
    // Closes the tabs `pick` selects in the active view (Close Multiple Documents).
    void closeTabsWhere(const std::function<bool(Tab& t, int index, int activeIndex)>& pick);
    bool confirmSave(core::Buffer& b);       // asks to save; false = cancel
    bool closeTab(Tab& t, bool keepOne = true); // false if the user cancelled
    bool closeAll();
    void setLanguage(core::Buffer& b, const core::Language& lang);
    void setUserLanguage(core::Buffer& b, const core::UserLanguage& udl);
    void reapplyLanguage(core::Buffer& b);
    void applyBufferLanguage(EditorView& v, const core::Buffer& b);
    void languageExtras(EditorView& v, const core::Buffer& b); // auto-completion file, SQL escapes
    void nameAfterFirstLine(core::Buffer& b, EditorView& v);
    void openFolder(const std::filesystem::path& dir);        // dropped or given folder
    // Upstream order: a UDL's extensions, Style Configurator "User ext.",
    // then the built-in languages.
    void detectLanguage(core::Buffer& b, const std::filesystem::path& p);
    void loadUserLanguages();
    void setReadOnly(core::Buffer& b, bool ro);
    void markForcedDirty(core::Buffer& b, bool dirty);

    void watch(core::Buffer& b);
    void rewatchAll();
    void onFileEvent(core::Buffer& b);
    void scheduleChangeCheck();
    void restartSnapshotTimer();
    Tab* openSessionFile(const core::SessionFile& f, int viewIdx);
    bool quitConfirmed(); // requestQuit() between NPPN_BEFORESHUTDOWN and NPPN_CANCELSHUTDOWN
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

    // Commands (MainWindowCommands.cpp)
    void registerCommands();
    void registerFileCommands();
    void registerEditCommands();
    void registerSearchCommands();
    void registerViewCommands();
    void registerFormatCommands();
    void registerOtherCommands();
    // Runs `fn` on the active editor's Scintilla when there is one.
    void withSci(const std::function<void(const core::Sci&)>& fn);
    void editOp(const std::function<void(const core::Sci&)>& fn); // skipped for read-only documents
    void cmdOpen();
    void cmdLoadSession();
    void cmdSaveSession();
    void cmdImportConfig();
    void cmdGotoLine();
    void cmdAbout();
    void cmdSummary();
    void cmdSetTheme(const std::string& theme);
    void cmdConvertEncoding(const platform::TextEncoding& e);   // Encoding > Convert to ...
    void cmdReinterpret(const platform::TextEncoding& e);       // Encoding > UTF-8, Character Set
    void cmdSetEol(int scEolMode);
    void cmdSort(int id);
    void cmdFold(bool fold, int level);                         // level 0 = all
    void cmdFoldCurrent(bool fold);
    void cmdMultiSelect(bool all, bool matchCase, bool wholeWord);
    void cmdBeginEndSelect(bool column);
    void cmdOpenSelectedFile(bool folder);
    void cmdMarkToken(int style, bool all);
    void cmdJumpMarker(int indicator, bool forward);
    void cmdCopyStyled(const std::vector<int>& indicators);
    void cmdPostIt();
    void cmdRestoreRecent();
    void cmdOpenAllRecent();
    void openRecentFile(const std::string& file);
    void openUri(const std::string& uri);
    void openTerminal(const std::filesystem::path& dir);
    void sciCommand(unsigned msg);
    void moveTab(int to); // within the active view; -1 = end
    void showTabMenu(Tab& t, const GdkEvent* ev);
    const char* lineCommentFor(const core::Buffer& b) const;
    bool currentEncodingIs(const platform::TextEncoding& e) const;
    // nativeLang text with an English fallback.
    std::string trMessage(const char* box, const char* attr, const char* fallback) const
    {
        return tr_.message(box, attr, fallback);
    }

    // Plugins (MainWindowPlugins.cpp)
    struct PluginPanel {
        std::string dockId;   // "plugin:<module>:<dlgID>"
        std::string module;
        int dlgId = 0;
        GtkWidget* widget = nullptr;      // DockedWidgetData::hClient
        const wchar_t* name = nullptr;    // the plugin's own strings, re-read on NPPM_DMMUPDATEDISPINFO
        const wchar_t* addInfo = nullptr;
        unsigned mask = 0;
    };
    void refreshPluginMenu();
    core::KeyCombo pluginKey(const PluginManager::Command& c) const;
    // hwndFrom = the Notepad++ handle, idFrom = the buffer (0 for none).
    void notifyPlugins(unsigned code, const core::Buffer* b = nullptr);
    void notifyPlugins(unsigned code, const void* hwndFrom, uptr_t idFrom);
    void forwardScintillaNotification(const Tab& t, const SCNotification& n);
    void checkBufferActivated();
    void shutdownPlugins();   // NPPN_SHUTDOWN once, before the window goes
    core::Buffer* bufferFromId(uptr_t id) const;
    int langTypeOf(const core::Buffer& b) const;
    bool setLangType(core::Buffer& b, int langType);
    void setPluginCheck(int cmdId, bool on);
    void addPluginToolbarButton(int cmdId, const char* icon, const char* darkIcon);
    void refreshPluginToolbarButton(int cmdId);
    sptr_t pluginDockMessage(unsigned msg, uptr_t wParam, sptr_t lParam);
    PluginPanel* pluginPanel(const void* widget);
    void onPluginPanelShown(const std::string& id, bool shown);
    void cmdPluginsAdmin();
    void cmdImportPlugins();
    std::vector<std::filesystem::path> choosePluginFiles(GtkWindow* parent);

    // Macros and Run (MainWindowMacros.cpp)
    void registerMacroCommands();
    void recordScintilla(int message, uptr_t wParam, sptr_t lParam);
    void refreshMacroState();
    void refreshMacroMenu();
    void refreshRunMenu();
    std::vector<std::pair<std::string, core::KeyCombo>> extraAccels() const;
    void cmdSaveMacro();
    void cmdRunMultiMacro();
    void cmdRun();
    void cmdShortcutMapper(int tab); // MainWindowDialogs.cpp
    void cmdPreferences();           // MainWindowPrefs.cpp

    // Preferences in effect (MainWindowPrefs.cpp)
    void updateEditorOptions();
    void applyDarkMode();
    void applyTabBar();
    void applyToolbarIcons();
    void syncEditStates();
    static void showToolbarIcon(GtkWidget* item);
    std::filesystem::path dialogFolder() const; // Preferences > Default Directory
    bool desktopPrefersDark() const;
    const core::ApiFile* apiFor(const core::Buffer& b);
    void cmdColumnEditor();

    // Docking panels (MainWindowPanels.cpp)
    GtkWidget* buildPanels(GtkWidget* center); // returns the dock's root
    void registerPanelCommands();
    void refreshPanels();           // the active document or its state changed
    void refreshDocList();
    void onPanelShown(const std::string& id, bool shown);
    void restorePanels();           // docking layout, workspaces (settings)
    void savePanels();
    void togglePanel(const char* id);
    void toggleTerminal();
    void findInProjects(const std::vector<std::filesystem::path>& files);

    // Signal trampolines
    static void onStatefulAction(GSimpleAction* a, GVariant* value, gpointer self);
    static void onOpenRecent(GSimpleAction* a, GVariant* param, gpointer self);
    static void onSwitchPage(GtkNotebook* nb, GtkWidget* page, guint, gpointer self);
    static gboolean onTabStripButton(GtkWidget* nb, GdkEventButton* ev, gpointer self);
    static gboolean onTabStripScroll(GtkWidget* nb, GdkEventScroll* ev, gpointer self);
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
    core::Translator tr_;
    std::vector<core::UserLanguage> udls_; // stable after construction (buffers point into it)
    bool darkTheme_ = false;
    CommandTable cmds_;
    DynamicMenus dyn_;
    core::CommandKeys keys_;
    core::ShortcutsFile shortcuts_;
    const bool useSession_;

    GtkWidget* window_ = nullptr;
    GtkWidget* vbox_ = nullptr;
    GtkWidget* menubar_ = nullptr;
    GtkWidget* toolbar_ = nullptr;
    GtkWidget* statusBar_ = nullptr;
    GtkWidget* paned_ = nullptr;
    GtkWidget* notebooks_[2] = {nullptr, nullptr};
    std::unique_ptr<FindBar> findBar_;      // Search > Incremental Search
    std::unique_ptr<SearchResults> results_;
    std::unique_ptr<FindDialog> findDlg_;
    core::FunctionParsers funcParsers_;
    std::unique_ptr<DockManager> dock_;
    std::unique_ptr<FunctionListPanel> funcList_;
    std::unique_ptr<FolderPanel> folder_;
    std::unique_ptr<DocumentMap> docMap_;
    std::unique_ptr<DocListPanel> docList_;
    std::unique_ptr<CharPanel> charPanel_;
    std::unique_ptr<ClipboardHistoryPanel> clipHistory_;
    std::unique_ptr<TerminalPanel> terminal_;
    std::array<std::unique_ptr<ProjectPanel>, 3> projects_;
    const EditorView* panelSource_ = nullptr; // what the document panels show
    std::string panelKey_;                    // document + language the Function List shows
    GtkWidget* stLang_ = nullptr;
    GtkWidget* stStats_ = nullptr;
    GtkWidget* stPos_ = nullptr;
    GtkWidget* stEol_ = nullptr;
    GtkWidget* stEnc_ = nullptr;
    GtkWidget* stIns_ = nullptr;
    GtkWidget* tabMenu_ = nullptr;
    std::vector<std::unique_ptr<Tab>> tabs_;
    std::unordered_map<const core::Buffer*, std::unique_ptr<platform::FileMonitor>> monitors_;
    int activeView_ = kMainView;
    double tabWheel_ = 0; // smooth scrolling on a tab bar, not a whole step yet
    std::filesystem::path lastDir_;
    PromptHandler prompt_;
    bool syncingActions_ = false;
    bool alwaysOnTop_ = false;
    bool fullScreen_ = false;
    bool postIt_ = false;
    sptr_t beginSelectPos_ = -1;   // Edit > Begin/End Select: the start, -1 = none
    bool recording_ = false;
    bool playing_ = false;
    bool inRecordedCommand_ = false; // a recorded menu command is running: skip its Scintilla steps
    core::Macro macro_;
    bool closing_ = false;         // tearing down: buffers keep their snapshots
    bool checkingChanges_ = false;
    std::optional<core::Session> lastWrittenSession_;
    guint refreshIdle_ = 0;
    guint changeCheckIdle_ = 0;
    guint snapshotTimer_ = 0;
    GtkPrintSettings* printSettings_ = nullptr; // kept between print jobs, as upstream keeps its DEVMODE

    EditorOptions editorOpts_;                    // Preferences for the editing aids
    std::unique_ptr<core::ApiCatalog> apis_;       // autoCompletion/*.xml
    GtkCssProvider* tabCss_ = nullptr;            // Preferences > Tab Bar
    GDBusConnection* colorSchemeBus_ = nullptr;   // dark mode following the desktop
    guint colorSchemeWatch_ = 0;
    bool altAlone_ = false;                       // Alt pressed with no other key: toggles a hidden menu
    std::vector<const core::Buffer*> mru_;         // Ctrl+Tab order, most recent first
    bool mruSwitching_ = false;                   // Ctrl+Tab cycling, until Ctrl is released

    std::unique_ptr<PluginManager> plugins_;
    std::vector<std::string> pluginErrors_;
    std::vector<PluginPanel> pluginPanels_;
    std::map<int, bool> pluginChecked_;           // plugin command -> check mark
    std::map<int, GtkToolItem*> pluginButtons_;    // plugin command -> toolbar button
    std::map<int, std::pair<std::string, std::string>> pluginIcons_; // command -> (icon, dark icon)
    const core::Buffer* activatedBuffer_ = nullptr; // last NPPN_BUFFERACTIVATED
    // During NPPN_FILEBEFORESAVE: the buffer, where it goes, and whether a
    // plugin stopped it (NPPM_GTKGETSAVETARGET, NPPM_GTKCANCELSAVE).
    const core::Buffer* savingBuffer_ = nullptr;
    std::filesystem::path saveTarget_;
    bool saveCancelled_ = false;
    std::string cmdLine_;
    std::filesystem::path exitSessionFile_;
    std::wstring pluginMessageArg_;               // -pluginMessage, alive while plugins may read it
    int modEventFlags_ = 0;                       // NPPM_ADDSCNMODIFIEDFLAGS
    bool pluginsShutDown_ = false;
    bool tabBarHidden_ = false;
    bool hidingPluginPanel_ = false;               // NPPM_DMMHIDE, not the user
};

} // namespace npp::ui
