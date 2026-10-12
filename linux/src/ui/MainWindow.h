// Main window — GTK counterpart of upstream Notepad_plus / Notepad_plus_Window
// (menus, tabs, status bar) for the skeleton feature set. See PORTING.md for
// how the remaining upstream modules map onto this structure.
#pragma once

#include "core/LanguageCatalog.h"
#include "core/StyleCatalog.h"
#include "platform/Settings.h"
#include "platform/TextCodec.h"
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
    MainWindow(GtkApplication* app, core::LanguageCatalog& langs);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    GtkWindow* window() const { return GTK_WINDOW(window_); }
    void present();

    void newDocument();
    // Opens files (or switches to them if already open). Returns how many opened.
    int openFiles(const std::vector<std::filesystem::path>& files, const OpenOptions& opts = {});

    // Introspection used by tests.
    std::size_t documentCount() const { return docs_.size(); }
    EditorView* activeView() const;
    std::string activeTitle() const;

private:
    struct Document {
        std::unique_ptr<EditorView> view;
        std::filesystem::path path;     // empty for "new N"
        std::string untitledName;
        platform::TextEncoding encoding;
        bool encodingChanged = false;   // modified via Encoding menu, no text edit
        GtkWidget* tabLabel = nullptr;
        GtkWidget* tabText = nullptr;

        bool isUntitled() const { return path.empty(); }
        bool isModified() const { return encodingChanged || view->isModified(); }
        std::string name() const { return isUntitled() ? untitledName : path.filename().string(); }
    };

    // Construction helpers
    void buildActions();
    void buildMenus(GtkWidget* vbox);
    void buildToolbar(GtkWidget* vbox);
    void buildStatusBar(GtkWidget* vbox);
    void loadStyles();
    std::vector<std::string> availableThemes() const;

    // Documents
    Document* addDocument(std::unique_ptr<Document> doc);
    Document* activeDoc() const;
    Document* docForPage(GtkWidget* page) const;
    Document* findByPath(const std::filesystem::path& p) const;
    bool loadInto(Document& doc, const std::filesystem::path& p, std::string* error);
    bool save(Document& doc, bool saveAs);
    bool closeDocument(Document& doc, bool keepOne = true); // false if user cancelled
    bool confirmClose(Document& doc);  // asks to save; false = cancel
    void setLanguage(Document& doc, const core::Language& lang);
    std::string nextUntitledName() const;

    // UI refresh
    void refreshTab(Document& doc);
    void refreshTitle();
    void refreshStatus();
    void syncActionStates();
    void applyViewSettings(EditorView& v);
    void showError(const std::string& primary, const std::string& secondary);

    // Commands
    void cmdOpen();
    void cmdGotoLine();
    void cmdAbout();
    void cmdSetTheme(const std::string& theme);
    void cmdSetEncoding(const std::string& id);
    void cmdSetEol(const std::string& id);
    bool cmdCloseAll();
    void sciCommand(unsigned msg);

    // Signal trampolines
    static void onAction(GSimpleAction* a, GVariant* param, gpointer self);
    static void onStatefulAction(GSimpleAction* a, GVariant* value, gpointer self);
    static void onSwitchPage(GtkNotebook*, GtkWidget* page, guint, gpointer self);
    static gboolean onDeleteEvent(GtkWidget*, GdkEvent*, gpointer self);
    static gboolean onKeyPress(GtkWidget* w, GdkEventKey* ev, gpointer self);
    static void onDragData(GtkWidget*, GdkDragContext* ctx, gint, gint, GtkSelectionData* data, guint, guint time, gpointer self);
    static gboolean onConfigure(GtkWidget*, GdkEventConfigure* ev, gpointer self);
    static gboolean onWindowState(GtkWidget*, GdkEventWindowState* ev, gpointer self);
    void openUriList(std::string_view uris);

    GtkApplication* app_;
    core::LanguageCatalog& langs_;
    core::StyleCatalog styles_;
    platform::Settings settings_;

    GtkWidget* window_ = nullptr;
    GtkWidget* notebook_ = nullptr;
    std::unique_ptr<FindBar> findBar_;
    GtkWidget* stLang_ = nullptr;
    GtkWidget* stStats_ = nullptr;
    GtkWidget* stPos_ = nullptr;
    GtkWidget* stEol_ = nullptr;
    GtkWidget* stEnc_ = nullptr;
    GtkWidget* stIns_ = nullptr;

    std::unordered_map<std::string, std::function<void()>> commands_;
    std::vector<std::unique_ptr<Document>> docs_;
    std::filesystem::path lastDir_;
    bool syncingActions_ = false;
    guint refreshIdle_ = 0;
};

} // namespace npp::ui
