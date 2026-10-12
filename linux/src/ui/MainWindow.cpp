#include "ui/MainWindow.h"

#include "core/NppConfig.h"
#include "platform/Paths.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

// ---- menu model helpers ----------------------------------------------------

void addItem(GMenu* m, const char* label, const char* action)
{
    g_menu_append(m, label, action);
}

void addTargetItem(GMenu* m, const std::string& label, const char* action, const std::string& target)
{
    GMenuItem* it = g_menu_item_new(label.c_str(), nullptr);
    g_menu_item_set_action_and_target_value(it, action, g_variant_new_string(target.c_str()));
    g_menu_append_item(m, it);
    g_object_unref(it);
}

GMenu* section(GMenu* parent)
{
    GMenu* s = g_menu_new();
    g_menu_append_section(parent, nullptr, G_MENU_MODEL(s));
    g_object_unref(s); // parent holds the reference
    return s;
}

GMenu* submenu(GMenu* parent, const char* label)
{
    GMenu* s = g_menu_new();
    g_menu_append_submenu(parent, label, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

std::string eolId(int mode)
{
    switch (mode) {
    case SC_EOL_CRLF: return "crlf";
    case SC_EOL_CR: return "cr";
    default: return "lf";
    }
}

const char* eolLabel(int mode)
{
    switch (mode) {
    case SC_EOL_CRLF: return "Windows (CR LF)";
    case SC_EOL_CR: return "Macintosh (CR)";
    default: return "Unix (LF)";
    }
}

std::string encodingId(const platform::TextEncoding& e)
{
    switch (e.bom) {
    case platform::Bom::Utf8: return "utf-8-bom";
    case platform::Bom::Utf16LE: return "utf-16le-bom";
    case platform::Bom::Utf16BE: return "utf-16be-bom";
    case platform::Bom::None: break;
    }
    std::string cs = e.charset;
    std::transform(cs.begin(), cs.end(), cs.begin(), [](unsigned char c) { return std::tolower(c); });
    return cs == "utf-8" ? "utf-8" : "other";
}

double luminance(std::uint32_t bgr)
{
    const double r = bgr & 0xFF, g = (bgr >> 8) & 0xFF, b = (bgr >> 16) & 0xFF;
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
}

// GMenu labels use mnemonics: keep underscores in file names literal.
std::string escapeMnemonic(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (c == '_')
            out += '_';
        out += c;
    }
    return out;
}

void setClipboard(const std::string& text)
{
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), static_cast<gint>(text.size()));
}

const char* saveBackupId(platform::SaveBackup b)
{
    switch (b) {
    case platform::SaveBackup::Simple: return "simple";
    case platform::SaveBackup::Verbose: return "verbose";
    case platform::SaveBackup::None: break;
    }
    return "none";
}

const char* changeDetectionId(platform::ChangeDetection c)
{
    switch (c) {
    case platform::ChangeDetection::Off: return "off";
    case platform::ChangeDetection::AutoUpdate: return "auto";
    case platform::ChangeDetection::Notify: break;
    }
    return "notify";
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

MainWindow::MainWindow(GtkApplication* app, core::LanguageCatalog& langs, bool useSession)
    : app_(app), langs_(langs), useSession_(useSession)
{
    importConfigOnFirstRun(settings_.load());
    recent_.setMax(static_cast<std::size_t>(std::max(settings_.maxRecentFiles, 0)));
    recent_.assign(settings_.recentFiles);

    window_ = gtk_application_window_new(app_);
    g_object_ref(window_); // keep alive until our destructor
    gtk_window_set_default_size(GTK_WINDOW(window_), settings_.width, settings_.height);
    if (settings_.maximized)
        gtk_window_maximize(GTK_WINDOW(window_));
    gtk_window_set_icon_name(GTK_WINDOW(window_), NPP_EXE_NAME);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(window_), FALSE);

    loadStyles();
    buildActions();

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), vbox);
    buildMenus(vbox);
    buildToolbar(vbox);

    // Two views side by side, as upstream's main and sub view.
    paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_ref(paned_); // read in the destructor, after GTK may have destroyed the window
    notebooks_[kMainView] = buildNotebook(kMainView);
    notebooks_[kSubView] = buildNotebook(kSubView);
    gtk_paned_pack1(GTK_PANED(paned_), notebooks_[kMainView], TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned_), notebooks_[kSubView], TRUE, FALSE);
    if (settings_.splitPosition > 0)
        gtk_paned_set_position(GTK_PANED(paned_), settings_.splitPosition);
    gtk_widget_set_vexpand(paned_, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), paned_, TRUE, TRUE, 0);

    findBar_ = std::make_unique<FindBar>([this] { return activeView(); });
    gtk_box_pack_start(GTK_BOX(vbox), findBar_->widget(), FALSE, FALSE, 0);
    buildStatusBar(vbox);

    static const GtkTargetEntry uriTarget{const_cast<gchar*>("text/uri-list"), 0, 0};
    gtk_drag_dest_set(window_, GTK_DEST_DEFAULT_ALL, &uriTarget, 1, GDK_ACTION_COPY);
    g_signal_connect(window_, "drag-data-received", G_CALLBACK(&MainWindow::onDragData), this);
    g_signal_connect(window_, "delete-event", G_CALLBACK(&MainWindow::onDeleteEvent), this);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(&MainWindow::onKeyPress), this);
    g_signal_connect(window_, "configure-event", G_CALLBACK(&MainWindow::onConfigure), this);
    g_signal_connect(window_, "window-state-event", G_CALLBACK(&MainWindow::onWindowState), this);
    g_signal_connect(window_, "notify::is-active", G_CALLBACK(&MainWindow::onActiveChanged), this);

    gtk_widget_show_all(vbox);
    gtk_widget_hide(notebooks_[kSubView]);
    refreshRecentMenu();
}

MainWindow::~MainWindow()
{
    closing_ = true;
    for (guint* src : {&refreshIdle_, &changeCheckIdle_, &snapshotTimer_})
        if (*src)
            g_source_remove(*src);
    if (paned_ && gtk_widget_get_visible(notebooks_[kSubView]) && gtk_widget_get_visible(notebooks_[kMainView]))
        settings_.splitPosition = gtk_paned_get_position(GTK_PANED(paned_));
    settings_.recentFiles = recent_.items();
    settings_.save();
    monitors_.clear();
    if (window_) {
        g_signal_handlers_disconnect_by_data(window_, this);
        for (GtkWidget* nb : notebooks_)
            if (nb)
                g_signal_handlers_disconnect_by_data(nb, this);
    }
    tabs_.clear(); // releases Scintilla widgets before the window goes
    if (tabMenu_) {
        gtk_widget_destroy(tabMenu_);
        g_object_unref(tabMenu_);
    }
    if (recentSection_)
        g_object_unref(recentSection_);
    if (window_)
        gtk_widget_destroy(window_);
    for (GtkWidget* w : {notebooks_[kMainView], notebooks_[kSubView], paned_})
        if (w)
            g_object_unref(w);
    if (window_)
        g_object_unref(window_);
}

GtkWidget* MainWindow::buildNotebook(int)
{
    GtkWidget* nb = gtk_notebook_new();
    g_object_ref(nb); // outlives the window's destruction, see ~MainWindow
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);
    gtk_notebook_popup_enable(GTK_NOTEBOOK(nb));
    gtk_widget_set_hexpand(nb, TRUE);
    g_signal_connect(nb, "switch-page", G_CALLBACK(&MainWindow::onSwitchPage), this);
    return nb;
}

void MainWindow::importConfigOnFirstRun(bool settingsExisted)
{
    // First start with a Notepad++ config.xml dropped into the config
    // directory: take its settings over, as upstream reads its own.
    const fs::path cfg = platform::userConfigDir() / "config.xml";
    std::error_code ec;
    if (settingsExisted || !fs::is_regular_file(cfg, ec))
        return;
    std::string err;
    if (auto c = core::readNppConfig(cfg, &err))
        core::applyNppConfig(*c, settings_);
    else
        g_warning("Ignoring %s", err.c_str());
}

void MainWindow::present()
{
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::loadStyles()
{
    std::string err;
    fs::path file;
    if (!settings_.theme.empty()) {
        for (const fs::path& dir : {platform::userConfigDir() / "themes", platform::dataDir() / "themes"}) {
            std::error_code ec;
            if (fs::is_regular_file(dir / settings_.theme, ec)) {
                file = dir / settings_.theme;
                break;
            }
        }
    }
    if (file.empty())
        file = platform::findConfigFile("stylers.xml", "stylers.model.xml");
    if (file.empty() || !styles_.load(file, &err)) {
        g_warning("Cannot load styles (%s); using Scintilla defaults", err.c_str());
        styles_ = core::StyleCatalog{};
    }
    // Follow the editor theme with the GTK chrome, like upstream dark mode.
    bool dark = false;
    if (const core::Style* def = styles_.globalById(STYLE_DEFAULT); def && def->bg)
        dark = luminance(*def->bg) < 0.4;
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, nullptr);
}

std::vector<std::string> MainWindow::availableThemes() const
{
    std::set<std::string> names;
    for (const fs::path& dir : {platform::dataDir() / "themes", platform::userConfigDir() / "themes"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".xml")
                names.insert(e.path().filename().string());
    }
    return {names.begin(), names.end()};
}

void MainWindow::buildActions()
{
    auto view = [this] { return activeView(); };
    auto buf = [this] { return activeBuffer(); };
    auto zoomAll = [this] {
        for (auto& t : tabs_)
            applyViewSettings(*t->view);
    };
    auto zoomFromActive = [this, zoomAll] {
        if (EditorView* v = activeView())
            settings_.zoom = static_cast<int>(v->execute(SCI_GETZOOM));
        zoomAll();
    };
    commands_ = {
        {"new", [this] { newDocument(); }},
        {"open", [this] { cmdOpen(); }},
        {"reload", [this, buf] {
             core::Buffer* b = buf();
             if (!b || b->isUntitled())
                 return;
             if (isModified(*b) && ask("Are you sure you want to reload the current file and lose the changes made in Notepad++?",
                                       b->path.string(), false) != Answer::Yes)
                 return;
             std::string err;
             if (!reloadBuffer(*b, &err))
                 showError("Cannot reload file", err);
         }},
        {"save", [this, buf] { if (core::Buffer* b = buf()) save(*b, false); }},
        {"save-as", [this, buf] { if (core::Buffer* b = buf()) save(*b, true); }},
        {"save-all", [this] {
             for (core::Buffer* b : buffers())
                 if (isModified(*b) && !save(*b, false))
                     break;
         }},
        {"close", [this] { if (Tab* t = activeTab()) closeTab(*t); }},
        {"close-all", [this] { closeAll(); }},
        {"close-others", [this] {
             Tab* keep = activeTab();
             if (!keep)
                 return;
             for (int vi : {kSubView, kMainView})
                 for (Tab* t : tabsInOrder(vi))
                     if (t->buf != keep->buf && !closeTab(*t, false))
                         return;
             for (Tab* t : tabsOf(*keep->buf)) // its clone goes too
                 if (t != keep)
                     removeTab(*t);
             activateTab(*keep);
         }},
        {"load-session", [this] { cmdLoadSession(); }},
        {"save-session", [this] { cmdSaveSession(); }},
        {"restore-recent", [this] { cmdRestoreRecent(); }},
        {"open-all-recent", [this] { cmdOpenAllRecent(); }},
        {"clear-recent", [this] { recent_.clear(); refreshRecentMenu(); }},
        {"quit", [this] { gtk_window_close(GTK_WINDOW(window_)); }},

        {"undo", [this] { sciCommand(SCI_UNDO); }},
        {"redo", [this] { sciCommand(SCI_REDO); }},
        {"cut", [this] { sciCommand(SCI_CUT); }},
        {"copy", [this] { sciCommand(SCI_COPY); }},
        {"paste", [this] { sciCommand(SCI_PASTE); }},
        {"delete", [this] { sciCommand(SCI_CLEAR); }},
        {"select-all", [this] { sciCommand(SCI_SELECTALL); }},
        {"duplicate", [this] { sciCommand(SCI_SELECTIONDUPLICATE); }},
        {"delete-line", [this] { sciCommand(SCI_LINEDELETE); }},
        {"move-up", [this] { sciCommand(SCI_MOVESELECTEDLINESUP); }},
        {"move-down", [this] { sciCommand(SCI_MOVESELECTEDLINESDOWN); }},
        {"upper", [this] { sciCommand(SCI_UPPERCASE); }},
        {"lower", [this] { sciCommand(SCI_LOWERCASE); }},
        {"toggle-comment", [view] { if (EditorView* v = view()) v->toggleLineComment(); }},
        {"copy-full-path", [buf] { if (core::Buffer* b = buf()) setClipboard(b->fullName()); }},
        {"copy-file-name", [buf] { if (core::Buffer* b = buf()) setClipboard(b->name()); }},
        {"copy-dir-path", [buf] { if (core::Buffer* b = buf(); b && !b->isUntitled()) setClipboard(b->path.parent_path().string()); }},
        {"open-folder", [this, buf] {
             core::Buffer* b = buf();
             if (!b || b->isUntitled())
                 return;
             gchar* uri = g_filename_to_uri(b->path.parent_path().c_str(), nullptr, nullptr);
             if (uri && !gtk_show_uri_on_window(GTK_WINDOW(window_), uri, GDK_CURRENT_TIME, nullptr))
                 showError("Cannot open the containing folder", b->path.parent_path().string());
             g_free(uri);
         }},

        {"find", [this] { findBar_->show(false); }},
        {"replace", [this] { findBar_->show(true); }},
        {"find-next", [this] { findBar_->findAgain(true); }},
        {"find-prev", [this] { findBar_->findAgain(false); }},
        {"goto", [this] { cmdGotoLine(); }},
        {"bookmark-toggle", [view] { if (EditorView* v = view()) v->toggleBookmarkAtCaret(); }},
        {"bookmark-next", [view] { if (EditorView* v = view()) v->gotoNextBookmark(true); }},
        {"bookmark-prev", [view] { if (EditorView* v = view()) v->gotoNextBookmark(false); }},

        {"zoom-in", [this, zoomFromActive] { sciCommand(SCI_ZOOMIN); zoomFromActive(); }},
        {"zoom-out", [this, zoomFromActive] { sciCommand(SCI_ZOOMOUT); zoomFromActive(); }},
        {"zoom-reset", [this, zoomAll] { settings_.zoom = 0; zoomAll(); }},
        {"next-tab", [this] {
             GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[activeView_]);
             const int n = gtk_notebook_get_n_pages(nb);
             if (n > 1)
                 gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + 1) % n);
         }},
        {"prev-tab", [this] {
             GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[activeView_]);
             const int n = gtk_notebook_get_n_pages(nb);
             if (n > 1)
                 gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + n - 1) % n);
         }},
        {"move-to-other-view", [this] { moveToOtherView(); }},
        {"clone-to-other-view", [this] { cloneToOtherView(); }},
        {"switch-view", [this] { switchToOtherView(); }},

        {"import-config", [this] { cmdImportConfig(); }},
        {"about", [this] { cmdAbout(); }},
    };
    for (const auto& [name, fn] : commands_) {
        GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(&MainWindow::onAction), this);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(a));
        g_object_unref(a);
    }
    GSimpleAction* recent = g_simple_action_new("open-recent", G_VARIANT_TYPE_STRING);
    g_signal_connect(recent, "activate", G_CALLBACK(&MainWindow::onOpenRecent), this);
    g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(recent));
    g_object_unref(recent);

    // Stateful actions (check / radio items).
    auto addState = [this](const char* name, GVariant* initial) {
        // Radio (string) actions take their target as parameter; toggles take none.
        const GVariantType* param = g_variant_is_of_type(initial, G_VARIANT_TYPE_STRING) ? G_VARIANT_TYPE_STRING : nullptr;
        GSimpleAction* a = g_simple_action_new_stateful(name, param, initial);
        g_signal_connect(a, "change-state", G_CALLBACK(&MainWindow::onStatefulAction), this);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(a));
        g_object_unref(a);
    };
    addState("word-wrap", g_variant_new_boolean(settings_.wordWrap));
    addState("show-all", g_variant_new_boolean(settings_.showAllChars));
    addState("indent-guides", g_variant_new_boolean(settings_.showIndentGuides));
    addState("language", g_variant_new_string("normal"));
    addState("encoding", g_variant_new_string("utf-8"));
    addState("eol", g_variant_new_string("lf"));
    addState("theme", g_variant_new_string(settings_.theme.c_str()));
    addState("read-only", g_variant_new_boolean(false));
    addState("remember-session", g_variant_new_boolean(settings_.rememberSession));
    addState("snapshot", g_variant_new_boolean(settings_.snapshot));
    addState("save-backup", g_variant_new_string(saveBackupId(settings_.saveBackup)));
    addState("change-detection", g_variant_new_string(changeDetectionId(settings_.changeDetection)));
    addState("replace-tabs", g_variant_new_boolean(settings_.replaceTabsBySpace));
    addState("auto-indent", g_variant_new_boolean(settings_.autoIndent));

    // Upstream default shortcuts (see shortcuts.xml / Shortcut Mapper).
    const std::pair<const char*, std::vector<const char*>> accels[] = {
        {"win.new", {"<Primary>n"}},
        {"win.open", {"<Primary>o"}},
        {"win.save", {"<Primary>s"}},
        {"win.save-as", {"<Primary><Alt>s"}},
        {"win.save-all", {"<Primary><Shift>s"}},
        {"win.close", {"<Primary>w"}},
        {"win.close-all", {"<Primary><Shift>w"}},
        {"win.undo", {"<Primary>z"}},
        {"win.redo", {"<Primary>y", "<Primary><Shift>z"}},
        {"win.cut", {"<Primary>x"}},
        {"win.copy", {"<Primary>c"}},
        {"win.paste", {"<Primary>v"}},
        {"win.select-all", {"<Primary>a"}},
        {"win.duplicate", {"<Primary>d"}},
        {"win.delete-line", {"<Primary>l"}},
        {"win.move-up", {"<Primary><Shift>Up"}},
        {"win.move-down", {"<Primary><Shift>Down"}},
        {"win.upper", {"<Primary><Shift>u"}},
        {"win.lower", {"<Primary>u"}},
        {"win.toggle-comment", {"<Primary>q"}},
        {"win.find", {"<Primary>f"}},
        {"win.replace", {"<Primary>h"}},
        {"win.find-next", {"F3"}},
        {"win.find-prev", {"<Shift>F3"}},
        {"win.goto", {"<Primary>g"}},
        {"win.bookmark-toggle", {"<Primary>F2"}},
        {"win.bookmark-next", {"F2"}},
        {"win.bookmark-prev", {"<Shift>F2"}},
        {"win.zoom-in", {"<Primary>plus", "<Primary>equal", "<Primary>KP_Add"}},
        {"win.zoom-out", {"<Primary>minus", "<Primary>KP_Subtract"}},
        {"win.zoom-reset", {"<Primary>0", "<Primary>KP_Divide"}},
        {"win.next-tab", {"<Primary>Tab", "<Primary>Page_Down"}},
        {"win.prev-tab", {"<Primary><Shift>ISO_Left_Tab", "<Primary>Page_Up"}},
        {"win.restore-recent", {"<Primary><Shift>t"}},
        {"win.switch-view", {"F8"}},
        {"win.reload", {}},
    };
    for (const auto& [action, keys] : accels) {
        std::vector<const char*> v = keys;
        v.push_back(nullptr);
        gtk_application_set_accels_for_action(app_, action, v.data());
    }
}

void MainWindow::buildMenus(GtkWidget* vbox)
{
    GMenu* bar = g_menu_new();

    GMenu* file = submenu(bar, "_File");
    GMenu* s = section(file);
    addItem(s, "_New", "win.new");
    addItem(s, "_Open…", "win.open");
    addItem(s, "Open Containing _Folder", "win.open-folder");
    addItem(s, "Re_load from Disk", "win.reload");
    s = section(file);
    addItem(s, "_Save", "win.save");
    addItem(s, "Save _As…", "win.save-as");
    addItem(s, "Sa_ve All", "win.save-all");
    s = section(file);
    addItem(s, "_Close", "win.close");
    addItem(s, "Clos_e All", "win.close-all");
    addItem(s, "Close All _but Active Document", "win.close-others");
    s = section(file);
    addItem(s, "Load Sess_ion…", "win.load-session");
    addItem(s, "Save Sessio_n…", "win.save-session");
    // Upstream keeps the history in the File menu itself.
    s = section(file);
    addItem(s, "Restore Recent Closed File", "win.restore-recent");
    addItem(s, "Open All Recent Files", "win.open-all-recent");
    addItem(s, "Empty Recent Files List", "win.clear-recent");
    recentSection_ = section(file);
    g_object_ref(recentSection_); // rebuilt by refreshRecentMenu()
    s = section(file);
    addItem(s, "E_xit", "win.quit");

    GMenu* edit = submenu(bar, "_Edit");
    s = section(edit);
    addItem(s, "_Undo", "win.undo");
    addItem(s, "_Redo", "win.redo");
    s = section(edit);
    addItem(s, "Cu_t", "win.cut");
    addItem(s, "_Copy", "win.copy");
    addItem(s, "_Paste", "win.paste");
    addItem(s, "_Delete", "win.delete");
    addItem(s, "Select A_ll", "win.select-all");
    s = section(edit);
    GMenu* lineOps = submenu(s, "Line Operations");
    addItem(lineOps, "Duplicate Current Line", "win.duplicate");
    addItem(lineOps, "Delete Current Line", "win.delete-line");
    addItem(lineOps, "Move Up Current Line", "win.move-up");
    addItem(lineOps, "Move Down Current Line", "win.move-down");
    GMenu* conv = submenu(s, "Convert Case to");
    addItem(conv, "_UPPERCASE", "win.upper");
    addItem(conv, "_lowercase", "win.lower");
    GMenu* cmt = submenu(s, "Comment/Uncomment");
    addItem(cmt, "Toggle Single Line Comment", "win.toggle-comment");
    GMenu* eol = submenu(s, "EOL Conversion");
    addTargetItem(eol, "Windows (CR LF)", "win.eol", "crlf");
    addTargetItem(eol, "Unix (LF)", "win.eol", "lf");
    addTargetItem(eol, "Macintosh (CR)", "win.eol", "cr");
    s = section(edit);
    GMenu* clip = submenu(s, "Copy to Clip_board");
    addItem(clip, "Copy Current Full File Path", "win.copy-full-path");
    addItem(clip, "Copy Current Filename", "win.copy-file-name");
    addItem(clip, "Copy Current Dir. Path", "win.copy-dir-path");
    addItem(s, "Read-_Only", "win.read-only");

    GMenu* search = submenu(bar, "_Search");
    s = section(search);
    addItem(s, "_Find…", "win.find");
    addItem(s, "Find _Next", "win.find-next");
    addItem(s, "Find _Previous", "win.find-prev");
    addItem(s, "_Replace…", "win.replace");
    addItem(s, "_Go To…", "win.goto");
    s = section(search);
    GMenu* bm = submenu(s, "Book_mark");
    addItem(bm, "Toggle Bookmark", "win.bookmark-toggle");
    addItem(bm, "Next Bookmark", "win.bookmark-next");
    addItem(bm, "Previous Bookmark", "win.bookmark-prev");

    GMenu* viewM = submenu(bar, "_View");
    s = section(viewM);
    addItem(s, "Word _wrap", "win.word-wrap");
    addItem(s, "Show All _Characters", "win.show-all");
    addItem(s, "Show _Indent Guide", "win.indent-guides");
    s = section(viewM);
    GMenu* zoom = submenu(s, "_Zoom");
    addItem(zoom, "Zoom _In", "win.zoom-in");
    addItem(zoom, "Zoom _Out", "win.zoom-out");
    addItem(zoom, "_Restore Default Zoom", "win.zoom-reset");
    s = section(viewM);
    addItem(s, "Next Tab", "win.next-tab");
    addItem(s, "Previous Tab", "win.prev-tab");
    s = section(viewM);
    addItem(s, "_Move to Other View", "win.move-to-other-view");
    addItem(s, "C_lone to Other View", "win.clone-to-other-view");
    addItem(s, "S_witch to Other View", "win.switch-view");

    GMenu* enc = submenu(bar, "E_ncoding");
    s = section(enc);
    addTargetItem(s, "UTF-8", "win.encoding", "utf-8");
    addTargetItem(s, "UTF-8-BOM", "win.encoding", "utf-8-bom");
    addTargetItem(s, "UTF-16 BE BOM", "win.encoding", "utf-16be-bom");
    addTargetItem(s, "UTF-16 LE BOM", "win.encoding", "utf-16le-bom");

    // Language menu grouped by initial, as upstream does.
    GMenu* lang = submenu(bar, "_Language");
    addTargetItem(section(lang), "Normal Text", "win.language", "normal");
    std::map<char, std::vector<const core::Language*>> byLetter;
    for (const core::Language& l : langs_.all())
        if (!l.isPlainText() && !l.shortName.empty())
            byLetter[static_cast<char>(std::toupper(static_cast<unsigned char>(l.shortName[0])))].push_back(&l);
    GMenu* letters = section(lang);
    for (auto& [letter, list] : byLetter) {
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return g_ascii_strcasecmp(a->shortName.c_str(), b->shortName.c_str()) < 0; });
        GMenu* sub = submenu(letters, std::string(1, letter).c_str());
        for (const core::Language* l : list)
            addTargetItem(sub, l->shortName, "win.language", l->name);
    }

    GMenu* settings = submenu(bar, "Se_ttings");
    GMenu* theme = submenu(settings, "_Theme");
    addTargetItem(section(theme), "Default (stylers.model.xml)", "win.theme", "");
    GMenu* themes = section(theme);
    for (const std::string& t : availableThemes())
        addTargetItem(themes, fs::path(t).stem().string(), "win.theme", t);
    // Subsets of upstream Preferences: Backup, MISC. and Indentation.
    s = section(settings);
    GMenu* backup = submenu(s, "_Backup");
    GMenu* bs = section(backup);
    addItem(bs, "Remember Current Session for Next Launch", "win.remember-session");
    addItem(bs, "Session Snapshot and Periodic Backup", "win.snapshot");
    bs = section(backup);
    addTargetItem(bs, "No Backup on Save", "win.save-backup", "none");
    addTargetItem(bs, "Simple Backup on Save (.bak)", "win.save-backup", "simple");
    addTargetItem(bs, "Verbose Backup on Save (nppBackup)", "win.save-backup", "verbose");
    GMenu* detect = submenu(s, "File Status _Auto-Detection");
    addTargetItem(detect, "Disabled", "win.change-detection", "off");
    addTargetItem(detect, "Enabled (Ask to Reload)", "win.change-detection", "notify");
    addTargetItem(detect, "Update Silently", "win.change-detection", "auto");
    GMenu* indent = submenu(s, "_Indentation");
    addItem(indent, "Replace Tabs by Space", "win.replace-tabs");
    addItem(indent, "Auto-Indent", "win.auto-indent");
    s = section(settings);
    addItem(s, "_Import Notepad++ config.xml…", "win.import-config");

    GMenu* help = submenu(bar, "_?");
    addItem(help, "_About Notepad++ (GTK)…", "win.about");

    GtkWidget* menubar = gtk_menu_bar_new_from_model(G_MENU_MODEL(bar));
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    g_object_unref(bar);

    // Tab context menu (upstream tabContextMenu defaults, abridged).
    GMenu* tm = g_menu_new();
    s = section(tm);
    addItem(s, "Close", "win.close");
    addItem(s, "Close All but This", "win.close-others");
    addItem(s, "Save", "win.save");
    addItem(s, "Save As…", "win.save-as");
    addItem(s, "Reload", "win.reload");
    s = section(tm);
    addItem(s, "Open Containing Folder", "win.open-folder");
    addItem(s, "Copy Full File Path", "win.copy-full-path");
    addItem(s, "Copy Filename", "win.copy-file-name");
    addItem(s, "Copy Dir. Path", "win.copy-dir-path");
    s = section(tm);
    addItem(s, "Read-Only", "win.read-only");
    s = section(tm);
    addItem(s, "Move to Other View", "win.move-to-other-view");
    addItem(s, "Clone to Other View", "win.clone-to-other-view");
    tabMenu_ = gtk_menu_new_from_model(G_MENU_MODEL(tm));
    g_object_ref_sink(tabMenu_);
    gtk_menu_attach_to_widget(GTK_MENU(tabMenu_), window_, nullptr); // resolves "win." actions
    g_object_unref(tm);
}

void MainWindow::buildToolbar(GtkWidget* vbox)
{
    GtkWidget* tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);
    const struct { const char* icon; const char* action; const char* tip; } items[] = {
        {"document-new", "win.new", "New"},           {"document-open", "win.open", "Open"},
        {"document-save", "win.save", "Save"},        {nullptr, nullptr, nullptr},
        {"edit-cut", "win.cut", "Cut"},               {"edit-copy", "win.copy", "Copy"},
        {"edit-paste", "win.paste", "Paste"},         {nullptr, nullptr, nullptr},
        {"edit-undo", "win.undo", "Undo"},            {"edit-redo", "win.redo", "Redo"},
        {nullptr, nullptr, nullptr},
        {"edit-find", "win.find", "Find"},            {"edit-find-replace", "win.replace", "Replace"},
        {"zoom-in", "win.zoom-in", "Zoom in"},        {"zoom-out", "win.zoom-out", "Zoom out"},
    };
    for (const auto& it : items) {
        GtkToolItem* ti;
        if (!it.icon) {
            ti = gtk_separator_tool_item_new();
        } else {
            ti = gtk_tool_button_new(nullptr, it.tip);
            gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(ti), it.icon);
            gtk_actionable_set_action_name(GTK_ACTIONABLE(ti), it.action);
            gtk_widget_set_tooltip_text(GTK_WIDGET(ti), it.tip);
        }
        gtk_toolbar_insert(GTK_TOOLBAR(tb), ti, -1);
    }
    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);
}

void MainWindow::buildStatusBar(GtkWidget* vbox)
{
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    auto field = [bar](bool expand, int minWidth) {
        GtkWidget* l = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(l, minWidth, -1);
        gtk_widget_set_margin_start(l, 6);
        gtk_widget_set_margin_end(l, 6);
        gtk_widget_set_margin_top(l, 2);
        gtk_widget_set_margin_bottom(l, 2);
        gtk_box_pack_start(GTK_BOX(bar), l, expand, expand, 0);
        gtk_box_pack_start(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
        return l;
    };
    stLang_ = field(TRUE, 160);
    stStats_ = field(FALSE, 220);
    stPos_ = field(FALSE, 300);
    stEol_ = field(FALSE, 120);
    stEnc_ = field(FALSE, 110);
    stIns_ = field(FALSE, 40);
}

// ============================================================================
// UI refresh
// ============================================================================

void MainWindow::refreshTab(Tab& t)
{
    const core::Buffer& b = *t.buf;
    const std::string label = (isModified(b) ? "*" : "") + b.name() + (b.userReadOnly ? " [RO]" : "");
    gtk_label_set_text(GTK_LABEL(t.labelText), label.c_str());
    gtk_widget_set_tooltip_text(t.label, b.fullName().c_str());
    gtk_notebook_set_menu_label_text(GTK_NOTEBOOK(notebooks_[t.viewIdx]), t.view->widget(), b.name().c_str());
}

void MainWindow::refreshBufferTabs(core::Buffer& b)
{
    for (Tab* t : tabsOf(b))
        refreshTab(*t);
}

void MainWindow::refreshTitle()
{
    std::string title = "Notepad++";
    if (const core::Buffer* b = activeBuffer())
        title = (isModified(*b) ? "*" : "") + b->fullName() + " - Notepad++";
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
}

void MainWindow::refreshStatus()
{
    Tab* t = activeTab();
    if (!t) {
        for (GtkWidget* l : {stLang_, stStats_, stPos_, stEol_, stEnc_, stIns_})
            gtk_label_set_text(GTK_LABEL(l), "");
        return;
    }
    EditorView& v = *t->view;
    const core::Language* lang = t->buf->language;
    gtk_label_set_text(GTK_LABEL(stLang_), lang ? lang->longName.c_str() : "Normal text file");

    char buf[160];
    g_snprintf(buf, sizeof buf, "length : %ld    lines : %ld", static_cast<long>(v.length()), static_cast<long>(v.lineCount()));
    gtk_label_set_text(GTK_LABEL(stStats_), buf);

    const sptr_t pos = v.currentPos();
    const sptr_t selStart = v.execute(SCI_GETSELECTIONSTART), selEnd = v.execute(SCI_GETSELECTIONEND);
    const long selChars = static_cast<long>(v.execute(SCI_COUNTCHARACTERS, static_cast<uptr_t>(selStart), selEnd));
    const long selLines = selEnd > selStart ? static_cast<long>(v.lineFromPos(selEnd) - v.lineFromPos(selStart) + 1) : 0;
    g_snprintf(buf, sizeof buf, "Ln : %ld    Col : %ld    Sel : %ld | %ld", static_cast<long>(v.lineFromPos(pos) + 1),
               static_cast<long>(v.execute(SCI_GETCOLUMN, static_cast<uptr_t>(pos)) + 1), selChars, selLines);
    gtk_label_set_text(GTK_LABEL(stPos_), buf);

    gtk_label_set_text(GTK_LABEL(stEol_), eolLabel(static_cast<int>(v.execute(SCI_GETEOLMODE))));
    gtk_label_set_text(GTK_LABEL(stEnc_), t->buf->encoding.displayName().c_str());
    gtk_label_set_text(GTK_LABEL(stIns_), v.execute(SCI_GETOVERTYPE) ? "OVR" : "INS");
}

void MainWindow::refreshAll()
{
    syncActionStates();
    refreshTitle();
    refreshStatus();
}

void MainWindow::refreshRecentMenu()
{
    if (!recentSection_)
        return;
    g_menu_remove_all(recentSection_);
    const auto& items = recent_.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        // Upstream numbering: 1-9 get a mnemonic.
        const std::string num = i < 9 ? "_" + std::to_string(i + 1) : std::to_string(i + 1);
        addTargetItem(recentSection_, num + ": " + escapeMnemonic(items[i]), "win.open-recent", items[i]);
    }
    for (const char* name : {"restore-recent", "open-all-recent", "clear-recent"})
        if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), name))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(a), !items.empty());
}

void MainWindow::syncActionStates()
{
    syncingActions_ = true;
    auto set = [this](const char* name, GVariant* v) {
        if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), name))
            g_simple_action_set_state(G_SIMPLE_ACTION(a), v);
        else
            g_variant_unref(g_variant_ref_sink(v));
    };
    if (Tab* t = activeTab()) {
        const core::Buffer& b = *t->buf;
        set("language", g_variant_new_string(b.language ? b.language->name.c_str() : "normal"));
        set("encoding", g_variant_new_string(encodingId(b.encoding).c_str()));
        set("eol", g_variant_new_string(eolId(static_cast<int>(t->view->execute(SCI_GETEOLMODE))).c_str()));
        set("read-only", g_variant_new_boolean(b.userReadOnly));
    }
    set("word-wrap", g_variant_new_boolean(settings_.wordWrap));
    set("show-all", g_variant_new_boolean(settings_.showAllChars));
    set("indent-guides", g_variant_new_boolean(settings_.showIndentGuides));
    set("theme", g_variant_new_string(settings_.theme.c_str()));
    set("remember-session", g_variant_new_boolean(settings_.rememberSession));
    set("snapshot", g_variant_new_boolean(settings_.snapshot));
    set("save-backup", g_variant_new_string(saveBackupId(settings_.saveBackup)));
    set("change-detection", g_variant_new_string(changeDetectionId(settings_.changeDetection)));
    set("replace-tabs", g_variant_new_boolean(settings_.replaceTabsBySpace));
    set("auto-indent", g_variant_new_boolean(settings_.autoIndent));
    const bool twoDocs = tabs_.size() > 1;
    if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), "move-to-other-view"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), twoDocs);
    if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), "switch-view"))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), tabCount(kMainView) > 0 && tabCount(kSubView) > 0);
    syncingActions_ = false;
}

void MainWindow::applyViewSettings(EditorView& v)
{
    v.execute(SCI_SETWRAPMODE, settings_.wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE);
    v.execute(SCI_SETVIEWWS, settings_.showAllChars ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
    v.execute(SCI_SETVIEWEOL, settings_.showAllChars);
    v.execute(SCI_SETINDENTATIONGUIDES, settings_.showIndentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
    v.execute(SCI_SETZOOM, static_cast<uptr_t>(settings_.zoom));
    v.execute(SCI_SETTABWIDTH, static_cast<uptr_t>(settings_.tabSize));
    v.execute(SCI_SETUSETABS, !settings_.replaceTabsBySpace);
    v.autoIndent = settings_.autoIndent;
}

void MainWindow::showError(const std::string& primary, const std::string& secondary)
{
    if (prompt_) { // tests: no modal dialogs
        g_warning("%s: %s", primary.c_str(), secondary.c_str());
        return;
    }
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", primary.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", secondary.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

MainWindow::Answer MainWindow::ask(const std::string& primary, const std::string& secondary, bool cancellable)
{
    if (prompt_)
        return prompt_(primary, secondary, cancellable);
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                            primary.c_str());
    if (!secondary.empty())
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", secondary.c_str());
    if (cancellable)
        gtk_dialog_add_button(GTK_DIALOG(dlg), "_Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), "_No", GTK_RESPONSE_NO, "_Yes", GTK_RESPONSE_YES, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
    const gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r == GTK_RESPONSE_YES)
        return Answer::Yes;
    if (r == GTK_RESPONSE_NO || !cancellable)
        return Answer::No;
    return Answer::Cancel;
}

// ============================================================================
// Commands
// ============================================================================

void MainWindow::sciCommand(unsigned msg)
{
    if (EditorView* v = activeView())
        v->execute(msg);
}

void MainWindow::cmdOpen()
{
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new("Open", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    if (core::Buffer* b = activeBuffer(); b && !b->isUntitled())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), b->path.parent_path().c_str());
    else if (!lastDir_.empty())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), lastDir_.c_str());
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        std::vector<fs::path> files;
        GSList* list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList* l = list; l; l = l->next) {
            files.emplace_back(static_cast<char*>(l->data));
            g_free(l->data);
        }
        g_slist_free(list);
        openFiles(files);
    }
    g_object_unref(dlg);
}

void MainWindow::cmdGotoLine()
{
    EditorView* v = activeView();
    if (!v)
        return;
    GtkWidget* dlg = gtk_dialog_new_with_buttons("Go To...", GTK_WINDOW(window_), GTK_DIALOG_MODAL, "_Cancel",
                                                 GTK_RESPONSE_CANCEL, "_Go", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    char buf[96];
    g_snprintf(buf, sizeof buf, "Go to line (1 - %ld):", static_cast<long>(v->lineCount()));
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(buf), FALSE, FALSE, 4);
    GtkWidget* spin = gtk_spin_button_new_with_range(1, static_cast<double>(v->lineCount()), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), static_cast<double>(v->lineFromPos(v->currentPos()) + 1));
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 4);
    gtk_widget_show_all(box);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
        v->gotoLine(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) - 1);
    }
    gtk_widget_destroy(dlg);
    gtk_widget_grab_focus(v->widget());
}

void MainWindow::cmdAbout()
{
    const char* authors[] = {"Don Ho and the Notepad++ contributors (upstream)", "Linux GTK port contributors", nullptr};
    gtk_show_about_dialog(GTK_WINDOW(window_),
                          "program-name", "Notepad++ (GTK port preview)",
                          "version", NPP_VERSION,
                          "comments", "Unofficial native Linux build of the Notepad++ source tree.\n"
                                      "Editor component: Scintilla " NPP_SCINTILLA_VERSION ", Lexilla " NPP_LEXILLA_VERSION
                                      " (GTK 3 backend).",
                          "website", "https://notepad-plus-plus.org/",
                          "license-type", GTK_LICENSE_GPL_3_0,
                          "logo-icon-name", NPP_EXE_NAME,
                          "authors", authors,
                          nullptr);
}

void MainWindow::cmdSetTheme(const std::string& theme)
{
    settings_.theme = theme;
    loadStyles();
    for (core::Buffer* b : buffers())
        if (b->language)
            setLanguage(*b, *b->language);
}

void MainWindow::cmdSetEncoding(const std::string& id)
{
    core::Buffer* b = activeBuffer();
    if (!b)
        return;
    platform::TextEncoding e;
    if (id == "utf-8-bom")
        e = {"UTF-8", platform::Bom::Utf8};
    else if (id == "utf-16le-bom")
        e = {"UTF-16LE", platform::Bom::Utf16LE};
    else if (id == "utf-16be-bom")
        e = {"UTF-16BE", platform::Bom::Utf16BE};
    else
        e = {"UTF-8", platform::Bom::None};
    if (e == b->encoding)
        return;
    // Text is held as UTF-8, so this is upstream's "Convert to ...": the bytes
    // on disk change on the next save.
    b->encoding = e;
    markForcedDirty(*b, true);
    refreshStatus();
}

void MainWindow::cmdSetEol(const std::string& id)
{
    EditorView* v = activeView();
    if (!v)
        return;
    const int mode = id == "crlf" ? SC_EOL_CRLF : id == "cr" ? SC_EOL_CR : SC_EOL_LF;
    v->setEolMode(mode, true);
    refreshStatus();
}

namespace {

// Runs a native file chooser; returns the chosen path or empty.
fs::path chooseFile(GtkWindow* parent, const char* title, GtkFileChooserAction action, const char* accept,
                    const fs::path& folder, const char* pattern, const char* patternName)
{
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(title, parent, action, accept, "_Cancel");
    if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (!folder.empty())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), folder.c_str());
    if (pattern) {
        GtkFileFilter* f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, patternName);
        gtk_file_filter_add_pattern(f, pattern);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
        GtkFileFilter* all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "All files");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    }
    fs::path out;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
            out = fn;
            g_free(fn);
        }
    g_object_unref(dlg);
    return out;
}

} // namespace

void MainWindow::cmdLoadSession()
{
    const fs::path f = chooseFile(GTK_WINDOW(window_), "Load Session", GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", lastDir_,
                                  "*.xml", "Session files (*.xml)");
    std::string err;
    if (!f.empty() && !loadSession(f, &err))
        showError("Cannot load session", err);
}

void MainWindow::cmdSaveSession()
{
    const fs::path f = chooseFile(GTK_WINDOW(window_), "Save Session", GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", lastDir_,
                                  "*.xml", "Session files (*.xml)");
    std::string err;
    if (!f.empty() && !saveSession(f, &err))
        showError("Cannot save session", err);
}

void MainWindow::cmdImportConfig()
{
    const fs::path f = chooseFile(GTK_WINDOW(window_), "Import Notepad++ config.xml", GTK_FILE_CHOOSER_ACTION_OPEN, "_Import",
                                  platform::userConfigDir(), "config*.xml", "Notepad++ configuration (config.xml)");
    if (f.empty())
        return;
    std::string err;
    auto cfg = core::readNppConfig(f, &err);
    if (!cfg) {
        showError("Cannot import settings", err);
        return;
    }
    const std::string oldTheme = settings_.theme;
    const int n = core::applyNppConfig(*cfg, settings_);
    recent_.setMax(static_cast<std::size_t>(std::max(settings_.maxRecentFiles, 0)));
    recent_.assign(settings_.recentFiles);
    refreshRecentMenu();
    if (settings_.theme != oldTheme)
        cmdSetTheme(settings_.theme);
    for (auto& t : tabs_)
        applyViewSettings(*t->view);
    rewatchAll();
    restartSnapshotTimer();
    refreshAll();
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "Imported %d setting%s from %s.", n, n == 1 ? "" : "s", f.filename().c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

void MainWindow::cmdRestoreRecent()
{
    if (!recent_.empty())
        openRecentFile(recent_.items().front());
}

void MainWindow::cmdOpenAllRecent()
{
    std::vector<fs::path> files;
    for (const std::string& f : recent_.items()) {
        std::error_code ec;
        if (fs::is_regular_file(f, ec))
            files.emplace_back(f);
    }
    openFiles(files);
}

void MainWindow::openRecentFile(const std::string& file)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        if (ask("\"" + file + "\" doesn't exist anymore.", "Remove it from the recent files list?", false) == Answer::Yes) {
            recent_.remove(file);
            refreshRecentMenu();
        }
        return;
    }
    openFiles({file});
}

void MainWindow::showTabMenu(Tab& t, const GdkEvent* ev)
{
    activateTab(t); // the menu acts on the active document
    gtk_menu_popup_at_pointer(GTK_MENU(tabMenu_), ev);
}

void MainWindow::openUriList(std::string_view uris)
{
    std::vector<fs::path> files;
    gchar** list = g_uri_list_extract_uris(std::string(uris).c_str());
    for (gchar** u = list; u && *u; ++u)
        if (gchar* fn = g_filename_from_uri(*u, nullptr, nullptr)) {
            files.emplace_back(fn);
            g_free(fn);
        }
    g_strfreev(list);
    if (!files.empty())
        openFiles(files);
}

// ============================================================================
// Signal trampolines
// ============================================================================

void MainWindow::onAction(GSimpleAction* a, GVariant*, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    if (auto it = w->commands_.find(g_action_get_name(G_ACTION(a))); it != w->commands_.end())
        it->second();
}

void MainWindow::onStatefulAction(GSimpleAction* a, GVariant* value, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    g_simple_action_set_state(a, value);
    if (w->syncingActions_)
        return;
    const std::string name = g_action_get_name(G_ACTION(a));
    platform::Settings& st = w->settings_;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        const bool on = g_variant_get_boolean(value);
        if (name == "read-only") {
            if (core::Buffer* b = w->activeBuffer())
                w->setReadOnly(*b, on);
            return;
        }
        if (name == "remember-session" || name == "snapshot") {
            (name == "snapshot" ? st.snapshot : st.rememberSession) = on;
            w->restartSnapshotTimer();
            return;
        }
        if (name == "word-wrap")
            st.wordWrap = on;
        else if (name == "show-all")
            st.showAllChars = on;
        else if (name == "indent-guides")
            st.showIndentGuides = on;
        else if (name == "replace-tabs")
            st.replaceTabsBySpace = on;
        else if (name == "auto-indent")
            st.autoIndent = on;
        for (auto& t : w->tabs_)
            w->applyViewSettings(*t->view);
        return;
    }
    const std::string s = g_variant_get_string(value, nullptr);
    if (name == "language") {
        if (core::Buffer* b = w->activeBuffer())
            if (const core::Language* l = w->langs_.byName(s))
                w->setLanguage(*b, *l);
    } else if (name == "encoding") {
        w->cmdSetEncoding(s);
    } else if (name == "eol") {
        w->cmdSetEol(s);
    } else if (name == "theme") {
        w->cmdSetTheme(s);
    } else if (name == "save-backup") {
        st.saveBackup = s == "simple" ? platform::SaveBackup::Simple : s == "verbose" ? platform::SaveBackup::Verbose : platform::SaveBackup::None;
    } else if (name == "change-detection") {
        st.changeDetection = s == "off" ? platform::ChangeDetection::Off : s == "auto" ? platform::ChangeDetection::AutoUpdate : platform::ChangeDetection::Notify;
        w->rewatchAll();
    }
}

void MainWindow::onOpenRecent(GSimpleAction*, GVariant* param, gpointer self)
{
    static_cast<MainWindow*>(self)->openRecentFile(g_variant_get_string(param, nullptr));
}

void MainWindow::onSwitchPage(GtkNotebook*, GtkWidget* page, guint, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    // Called during switch: current page is not updated yet, so resolve by widget.
    Tab* t = w->tabForPage(page);
    if (!t || w->closing_)
        return;
    if (t->viewIdx == w->activeView_)
        gtk_widget_grab_focus(page);
    // Defer refresh until GtkNotebook has updated its current page.
    if (!w->refreshIdle_)
        w->refreshIdle_ = g_idle_add(+[](gpointer p) -> gboolean {
            auto* win = static_cast<MainWindow*>(p);
            win->refreshIdle_ = 0;
            win->refreshAll();
            return G_SOURCE_REMOVE;
        }, w);
}

gboolean MainWindow::onDeleteEvent(GtkWidget*, GdkEvent*, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    if (!w->requestQuit())
        return TRUE; // cancelled: keep window open
    // GTK destroys the window next: stop reacting to its widgets.
    w->closing_ = true;
    if (gtk_widget_get_visible(w->notebooks_[kSubView]) && gtk_widget_get_visible(w->notebooks_[kMainView]))
        w->settings_.splitPosition = gtk_paned_get_position(GTK_PANED(w->paned_));
    w->settings_.recentFiles = w->recent_.items();
    w->settings_.save();
    return FALSE;
}

void MainWindow::onActiveChanged(GObject*, GParamSpec*, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    if (!gtk_window_is_active(GTK_WINDOW(w->window_)))
        return;
    for (const auto& t : w->tabs_)
        if (t->buf->externalChangePending) {
            w->scheduleChangeCheck();
            break;
        }
}

gboolean MainWindow::onKeyPress(GtkWidget* widget, GdkEventKey* ev, gpointer)
{
    // When a text entry (find bar, dialogs) has focus, let it handle the key
    // before window accelerators, so Ctrl+C/V/Z/A act on the entry.
    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(widget));
    if (focus && GTK_IS_EDITABLE(focus))
        return gtk_window_propagate_key_event(GTK_WINDOW(widget), ev);
    return FALSE;
}

void MainWindow::onDragData(GtkWidget*, GdkDragContext* ctx, gint, gint, GtkSelectionData* data, guint, guint time, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    const guchar* raw = gtk_selection_data_get_data(data);
    const gint len = gtk_selection_data_get_length(data);
    if (raw && len > 0)
        w->openUriList(std::string_view(reinterpret_cast<const char*>(raw), static_cast<std::size_t>(len)));
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

gboolean MainWindow::onConfigure(GtkWidget* widget, GdkEventConfigure*, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    if (!w->settings_.maximized)
        gtk_window_get_size(GTK_WINDOW(widget), &w->settings_.width, &w->settings_.height);
    return FALSE;
}

gboolean MainWindow::onWindowState(GtkWidget*, GdkEventWindowState* ev, gpointer self)
{
    static_cast<MainWindow*>(self)->settings_.maximized = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
}

} // namespace npp::ui
