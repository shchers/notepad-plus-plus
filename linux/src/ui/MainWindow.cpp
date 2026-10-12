#include "ui/MainWindow.h"

#include "core/NppConfig.h"
#include "platform/Paths.h"

#include "menuCmdID.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

void addTargetItem(GMenu* m, const std::string& label, const char* action, const std::string& target)
{
    appendItem(m, label, action, target.c_str());
}

const char* eolLabel(int mode)
{
    switch (mode) {
    case SC_EOL_CRLF: return "Windows (CR LF)";
    case SC_EOL_CR: return "Macintosh (CR)";
    default: return "Unix (LF)";
    }
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


} // namespace

// ============================================================================
// Construction
// ============================================================================

MainWindow::MainWindow(GtkApplication* app, core::LanguageCatalog& langs, bool useSession)
    : app_(app), langs_(langs), useSession_(useSession)
{
    importConfigOnFirstRun(settings_.load());
    loadUserLanguages();
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
    std::string err;
    if (!tr_.load(platform::dataDir() / "nativeLang", settings_.localization, &err))
        g_warning("Localization: %s", err.c_str());
    if (!settings_.localization.empty() && tr_.fileName() != settings_.localization)
        settings_.localization.clear();
    gtk_widget_set_default_direction(tr_.rtl() ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    buildActions();

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    vbox_ = vbox;
    gtk_container_add(GTK_CONTAINER(window_), vbox);
    buildMenus();
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
    // The editors in the middle of the dock, with the panels around them.
    gtk_box_pack_start(GTK_BOX(vbox), buildPanels(paned_), TRUE, TRUE, 0);

    findBar_ = std::make_unique<FindBar>([this] { return activeView(); }, tr_);
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

    FindHost host;
    host.activeView = [this] { return activeView(); };
    host.documents = [this] {
        std::vector<std::pair<std::string, EditorView*>> docs;
        for (int vi : {kMainView, kSubView})
            for (Tab* t : tabsInOrder(vi))
                if (vi == kMainView || !tabOf(*t->buf, kMainView)) // a clone once
                    docs.emplace_back(t->buf->fullName(), t->view.get());
        return docs;
    };
    host.viewForFile = [this](const fs::path& p) { return viewForFile(p); };
    host.currentDirectory = [this] {
        core::Buffer* b = activeBuffer();
        return b && !b->isUntitled() ? b->path.parent_path() : lastDir_;
    };
    host.showResults = [this] {
        showSearchResults(true);
        return results_.get();
    };
    host.confirm = [this](const std::string& primary, const std::string& secondary) {
        return ask(primary, secondary, false) == Answer::Yes;
    };
    host.afterEdit = [this] { refreshAll(); };
    host.record = [this](const core::MacroStep& step) {
        if (recording_)
            macro_.push_back(step);
    };
    findDlg_ = std::make_unique<FindDialog>(GTK_WINDOW(window_), tr_, settings_.find, std::move(host));

    gtk_widget_show_all(vbox);
    gtk_widget_hide(notebooks_[kSubView]);
    restorePanels();
    gtk_widget_set_visible(toolbar_, !settings_.hideToolbar);
    gtk_widget_set_visible(statusBar_, !settings_.hideStatusBar);
    refreshRecentMenu();
    reloadShortcuts();
}

MainWindow::~MainWindow()
{
    closing_ = true;
    findDlg_.reset();
    if (dock_ && !gtk_widget_in_destruction(window_))
        savePanels();
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
    docMap_.reset(); // before the documents it shows
    tabs_.clear();   // releases Scintilla widgets before the window goes
    results_.reset();
    funcList_.reset();
    folder_.reset();
    docList_.reset();
    charPanel_.reset();
    clipHistory_.reset();
    for (auto& p : projects_)
        p.reset();
    dock_.reset();
    if (tabMenu_) {
        gtk_widget_destroy(tabMenu_);
        g_object_unref(tabMenu_);
    }
    if (printSettings_)
        g_object_unref(printSettings_);
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

fs::path MainWindow::themePath(const std::string& theme) const
{
    // The user's copy (saved by the Style Configurator) wins over the shipped one.
    if (!theme.empty())
        for (const fs::path& dir : {platform::userConfigDir() / "themes", platform::dataDir() / "themes"}) {
            std::error_code ec;
            if (fs::is_regular_file(dir / theme, ec))
                return dir / theme;
        }
    return platform::findConfigFile("stylers.xml", "stylers.model.xml");
}

void MainWindow::loadStyles()
{
    std::string err;
    const fs::path file = themePath(settings_.theme);
    if (file.empty() || !styles_.load(file, &err)) {
        g_warning("Cannot load styles (%s); using Scintilla defaults", err.c_str());
        styles_ = core::StyleCatalog{};
    }
    // Follow the editor theme with the GTK chrome, like upstream dark mode.
    bool dark = false;
    if (const core::Style* def = styles_.globalById(STYLE_DEFAULT); def && def->bg)
        dark = luminance(*def->bg) < 0.4;
    darkTheme_ = dark;
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
    registerCommands();
    cmds_.install(G_ACTION_MAP(window_));

    GSimpleAction* recent = g_simple_action_new("open-recent", G_VARIANT_TYPE_STRING);
    g_signal_connect(recent, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant* param, gpointer self) {
                         static_cast<MainWindow*>(self)->openRecentFile(g_variant_get_string(param, nullptr));
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(recent));
    g_object_unref(recent);

    // Radio groups filled at run time (Language menu, themes).
    for (const auto& [name, initial] : {std::pair<const char*, std::string>{"language", "normal"}, {"theme", settings_.theme}}) {
        GSimpleAction* a = g_simple_action_new_stateful(name, G_VARIANT_TYPE_STRING, g_variant_new_string(initial.c_str()));
        g_signal_connect(a, "change-state", G_CALLBACK(&MainWindow::onStatefulAction), this);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(a));
        g_object_unref(a);
    }
}

bool MainWindow::commandChecked(int id) const
{
    GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), CommandTable::actionName(id).c_str());
    if (!a)
        return false;
    GVariant* st = g_action_get_state(a);
    if (!st)
        return false;
    const bool on = g_variant_is_of_type(st, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(st);
    g_variant_unref(st);
    return on;
}

void MainWindow::buildMenus()
{
    GMenu* bar = buildMenuBar(tr_, cmds_, dyn_);
    GtkWidget* menubar = gtk_menu_bar_new_from_model(G_MENU_MODEL(bar));
    g_object_unref(bar);
    if (menubar_)
        gtk_widget_destroy(menubar_);
    menubar_ = menubar;
    gtk_box_pack_start(GTK_BOX(vbox_), menubar_, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(vbox_), menubar_, 0);
    gtk_widget_show_all(menubar_);
    refreshLanguageMenu();
    refreshThemeMenu();
    refreshRecentMenu();
    refreshMacroMenu();
    refreshRunMenu();

    GMenu* tm = buildTabMenu(tr_, cmds_);
    if (tabMenu_) {
        gtk_widget_destroy(tabMenu_);
        g_object_unref(tabMenu_);
    }
    tabMenu_ = gtk_menu_new_from_model(G_MENU_MODEL(tm));
    g_object_ref_sink(tabMenu_);
    gtk_menu_attach_to_widget(GTK_MENU(tabMenu_), window_, nullptr); // resolves "win." actions
    g_object_unref(tm);
}

void MainWindow::refreshLanguageMenu()
{
    if (!dyn_.languages)
        return;
    g_menu_remove_all(dyn_.languages);
    // Upstream groups languages by initial (Preferences > Language > "Make
    // language menu compact" is the default).
    addTargetItem(dyn_.languages, tr_.command(IDM_LANG_TEXT, "Normal Text"), "win.language", "normal");
    std::map<char, std::vector<const core::Language*>> byLetter;
    for (const core::Language& l : langs_.all())
        if (!l.isPlainText() && !l.shortName.empty())
            byLetter[static_cast<char>(std::toupper(static_cast<unsigned char>(l.shortName[0])))].push_back(&l);
    for (auto& [letter, list] : byLetter) {
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return g_ascii_strcasecmp(a->shortName.c_str(), b->shortName.c_str()) < 0; });
        GMenu* sub = g_menu_new();
        for (const core::Language* l : list)
            addTargetItem(sub, escapeMnemonic(l->shortName), "win.language", l->name);
        g_menu_append_submenu(dyn_.languages, std::string(1, letter).c_str(), G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
    if (dyn_.userLanguages) {
        g_menu_remove_all(dyn_.userLanguages);
        for (const core::UserLanguage& u : udls_)
            addTargetItem(dyn_.userLanguages, escapeMnemonic(u.name), "win.language", "udl:" + u.name);
    }
}

void MainWindow::loadUserLanguages()
{
    // Upstream: userDefineLang.xml and userDefineLangs\*.xml in the settings
    // folder; the port also reads the ones it ships (the bundled Markdown).
    udls_ = core::loadAllUserLanguages(platform::userConfigDir(),
                                       {platform::userConfigDir() / "userDefineLangs", platform::dataDir() / "userDefineLangs"});
}

const core::UserLanguage* MainWindow::userLanguage(const std::string& name) const
{
    for (const core::UserLanguage& u : udls_)
        if (u.name == name)
            return &u;
    return nullptr;
}

void MainWindow::refreshThemeMenu()
{
    if (!dyn_.themes)
        return;
    g_menu_remove_all(dyn_.themes);
    GMenu* sub = g_menu_new();
    addTargetItem(sub, "Default (stylers.model.xml)", "win.theme", "");
    for (const std::string& t : availableThemes())
        addTargetItem(sub, escapeMnemonic(fs::path(t).stem().string()), "win.theme", t);
    g_menu_append_submenu(dyn_.themes, "Theme", G_MENU_MODEL(sub));
    g_object_unref(sub);
}

void MainWindow::applyShortcuts()
{
    cmds_.applyAccels(app_, keys_, extraAccels());
}

void MainWindow::reloadShortcuts()
{
    keys_ = core::CommandKeys{};
    shortcuts_ = core::ShortcutsFile{};
    const fs::path file = platform::userConfigDir() / "shortcuts.xml";
    std::error_code ec;
    std::string err;
    if (fs::is_regular_file(file, ec)) {
        if (shortcuts_.load(file, &err))
            keys_.apply(shortcuts_.commands);
        else
            g_warning("Ignoring %s", err.c_str());
    }
    refreshMacroMenu();
    refreshRunMenu();
    applyShortcuts();
}

bool MainWindow::saveShortcuts(std::string* error)
{
    shortcuts_.commands = keys_.changes();
    return shortcuts_.save(platform::userConfigDir() / "shortcuts.xml", error);
}

void MainWindow::showSearchResults(bool show)
{
    if (show) {
        dock_->show(kSearchResultsPanel, false);
    } else {
        dock_->hide(kSearchResultsPanel);
        if (EditorView* v = activeView())
            gtk_widget_grab_focus(v->widget());
    }
}

EditorView* MainWindow::viewForFile(const fs::path& p) const
{
    std::error_code ec;
    const fs::path n = fs::weakly_canonical(p, ec);
    core::Buffer* b = findByPath(ec ? p : n);
    if (!b)
        return nullptr;
    if (Tab* t = tabOf(*b, activeView_))
        return t->view.get();
    return tabsOf(*b).front()->view.get();
}

void MainWindow::openResult(const ResultLocation& loc)
{
    Tab* t = nullptr;
    for (core::Buffer* b : buffers())
        if (b->fullName() == loc.file) {
            t = tabOf(*b, activeView_) ? tabOf(*b, activeView_) : tabsOf(*b).front();
            break;
        }
    if (!t) {
        std::error_code ec;
        if (!fs::is_regular_file(loc.file, ec) || openFiles({loc.file}) == 0)
            return;
        t = activeTab();
    }
    if (!t)
        return;
    activateTab(*t);
    if (loc.line < 0)
        return;
    EditorView& v = *t->view;
    v.gotoLine(loc.line);
    const sptr_t ls = v.execute(SCI_POSITIONFROMLINE, static_cast<uptr_t>(loc.line));
    const sptr_t le = v.execute(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(loc.line));
    v.execute(SCI_SETSEL, static_cast<uptr_t>(std::min(ls + loc.startInLine, le)), std::min(ls + loc.endInLine, le));
    v.execute(SCI_SCROLLCARET);
}

void MainWindow::setLocalization(const std::string& fileName)
{
    std::string err;
    if (!tr_.load(platform::dataDir() / "nativeLang", fileName, &err))
        showError("Cannot load the translation", err);
    settings_.localization = tr_.fileName() == "english.xml" ? std::string() : tr_.fileName();
    gtk_widget_set_default_direction(tr_.rtl() ? GTK_TEXT_DIR_RTL : GTK_TEXT_DIR_LTR);
    buildMenus();
    findDlg_->retranslate();
    findBar_->retranslate();
    syncActionStates();
}

void MainWindow::buildToolbar(GtkWidget* vbox)
{
    GtkWidget* tb = gtk_toolbar_new();
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_ICONS);
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);
    const struct { const char* icon; int id; const char* tip; } items[] = {
        {"document-new", IDM_FILE_NEW, "New"},          {"document-open", IDM_FILE_OPEN, "Open"},
        {"document-save", IDM_FILE_SAVE, "Save"},       {"document-save-as", IDM_FILE_SAVEALL, "Save All"},
        {"window-close", IDM_FILE_CLOSE, "Close"},      {"document-print", IDM_FILE_PRINT, "Print"},
        {nullptr, 0, nullptr},
        {"edit-cut", IDM_EDIT_CUT, "Cut"},              {"edit-copy", IDM_EDIT_COPY, "Copy"},
        {"edit-paste", IDM_EDIT_PASTE, "Paste"},        {nullptr, 0, nullptr},
        {"edit-undo", IDM_EDIT_UNDO, "Undo"},           {"edit-redo", IDM_EDIT_REDO, "Redo"},
        {nullptr, 0, nullptr},
        {"edit-find", IDM_SEARCH_FIND, "Find"},         {"edit-find-replace", IDM_SEARCH_REPLACE, "Replace"},
        {nullptr, 0, nullptr},
        {"zoom-in", IDM_VIEW_ZOOMIN, "Zoom in"},        {"zoom-out", IDM_VIEW_ZOOMOUT, "Zoom out"},
        {nullptr, 0, nullptr},
        {"media-record", IDM_MACRO_STARTRECORDINGMACRO, "Start Recording"},
        {"media-playback-stop", IDM_MACRO_STOPRECORDINGMACRO, "Stop Recording"},
        {"media-playback-start", IDM_MACRO_PLAYBACKRECORDEDMACRO, "Playback"},
    };
    for (const auto& it : items) {
        if (it.icon && !cmds_.has(it.id))
            continue;
        GtkToolItem* ti;
        if (!it.icon) {
            ti = gtk_separator_tool_item_new();
        } else {
            const std::string tip = core::Translator::plain(tr_.command(it.id, it.tip));
            ti = gtk_tool_button_new(nullptr, tip.c_str());
            gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(ti), it.icon);
            gtk_actionable_set_action_name(GTK_ACTIONABLE(ti), CommandTable::detailedName(it.id).c_str());
            gtk_widget_set_tooltip_text(GTK_WIDGET(ti), tip.c_str());
        }
        gtk_toolbar_insert(GTK_TOOLBAR(tb), ti, -1);
    }
    gtk_box_pack_start(GTK_BOX(vbox), tb, FALSE, FALSE, 0);
    toolbar_ = tb;
}

void MainWindow::buildStatusBar(GtkWidget* vbox)
{
    statusBar_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(statusBar_), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(statusBar_), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), statusBar_, FALSE, FALSE, 0);

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
    if (t->buf->userLanguage) // upstream status bar text
        gtk_label_set_text(GTK_LABEL(stLang_), ("User Defined language file - " + t->buf->userLanguage->name).c_str());
    else
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
    refreshPanels();
}

void MainWindow::refreshRecentMenu()
{
    if (!dyn_.recent)
        return;
    g_menu_remove_all(dyn_.recent);
    const auto& items = recent_.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        // Upstream numbering: 1-9 get a mnemonic.
        const std::string num = i < 9 ? "_" + std::to_string(i + 1) : std::to_string(i + 1);
        addTargetItem(dyn_.recent, num + ": " + escapeMnemonic(items[i]), "win.open-recent", items[i]);
    }
    for (int id : {IDM_FILE_RESTORELASTCLOSEDFILE, IDM_OPEN_ALL_RECENT_FILE, IDM_CLEAN_RECENT_FILE_LIST})
        cmds_.setEnabled(id, !items.empty());
}

void MainWindow::syncActionStates()
{
    syncingActions_ = true;
    auto set = [this](const char* name, const std::string& v) {
        if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), name))
            g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_string(v.c_str()));
    };
    if (Tab* t = activeTab())
        set("language", t->buf->userLanguage ? "udl:" + t->buf->userLanguage->name : t->buf->language ? t->buf->language->name : "normal");
    set("theme", settings_.theme);
    cmds_.syncStates();
    const bool twoDocs = tabs_.size() > 1;
    cmds_.setEnabled(IDM_VIEW_GOTO_ANOTHER_VIEW, twoDocs);
    cmds_.setEnabled(IDM_VIEW_SWITCHTO_OTHER_VIEW, tabCount(kMainView) > 0 && tabCount(kSubView) > 0);
    syncingActions_ = false;
}

void MainWindow::applyViewSettings(EditorView& v)
{
    v.execute(SCI_SETWRAPMODE, settings_.wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE);
    v.execute(SCI_SETVIEWWS, settings_.showSpaces ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
    v.execute(SCI_SETVIEWEOL, settings_.showEol);
    v.execute(SCI_SETINDENTATIONGUIDES, settings_.showIndentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
    v.execute(SCI_SETWRAPVISUALFLAGS, settings_.showWrapSymbol ? SC_WRAPVISUALFLAG_END : SC_WRAPVISUALFLAG_NONE);
    v.execute(SCI_SETZOOM, static_cast<uptr_t>(settings_.zoom));
    v.execute(SCI_SETTABWIDTH, static_cast<uptr_t>(settings_.tabSize));
    v.execute(SCI_SETUSETABS, !settings_.replaceTabsBySpace);
    v.autoIndent = settings_.autoIndent;
    static const int kWrapIndent[] = {SC_WRAPINDENT_FIXED, SC_WRAPINDENT_SAME, SC_WRAPINDENT_INDENT};
    v.execute(SCI_SETWRAPINDENTMODE, static_cast<uptr_t>(kWrapIndent[std::clamp(settings_.wrapIndent, 0, 2)]));
    v.execute(SCI_SETCARETSTYLE, settings_.caretWidth == 0 ? CARETSTYLE_BLOCK : CARETSTYLE_LINE);
    v.execute(SCI_SETCARETWIDTH, static_cast<uptr_t>(std::max(settings_.caretWidth, 1)));
    v.execute(SCI_SETCARETPERIOD, static_cast<uptr_t>(settings_.caretBlinkMs));
    v.execute(SCI_SETENDATLASTLINE, !settings_.scrollBeyondLastLine);
    v.execute(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION | (settings_.virtualSpace ? SCVS_USERACCESSIBLE : 0));
    v.execute(SCI_SETCARETLINEVISIBLE, settings_.currentLineHighlight);
    v.setFoldStyle(settings_.foldStyle);
    v.setLineNumbersVisible(settings_.showLineNumbers);
    v.setBookmarkMarginVisible(settings_.showBookmarkMargin);
    static const int kEdge[] = {EDGE_NONE, EDGE_LINE, EDGE_BACKGROUND};
    v.execute(SCI_SETEDGEMODE, static_cast<uptr_t>(kEdge[std::clamp(settings_.edgeMode, 0, 2)]));
    v.execute(SCI_SETEDGECOLUMN, static_cast<uptr_t>(settings_.edgeColumn));
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
// Signal trampolines
// ============================================================================

void MainWindow::onStatefulAction(GSimpleAction* a, GVariant* value, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    g_simple_action_set_state(a, value);
    if (w->syncingActions_)
        return;
    const std::string name = g_action_get_name(G_ACTION(a));
    const std::string s = g_variant_get_string(value, nullptr);
    if (name == "language") {
        core::Buffer* b = w->activeBuffer();
        if (!b)
            return;
        if (s.rfind("udl:", 0) == 0) {
            if (const core::UserLanguage* u = w->userLanguage(s.substr(4)))
                w->setUserLanguage(*b, *u);
        } else if (const core::Language* l = w->langs_.byName(s)) {
            w->setLanguage(*b, *l);
        }
    } else if (name == "theme") {
        w->cmdSetTheme(s);
    }
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
    w->savePanels();
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
