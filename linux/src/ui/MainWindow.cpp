#include "ui/MainWindow.h"

#include "core/NppConfig.h"
#include "platform/Paths.h"

#include "menuCmdID.h"
#include <windows.h> // plugin SDK: NPPN_* codes
#include "Notepad_plus_msgs.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
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
    if (settings_.recentCheckAtLaunch) // Preferences: drop the files that are gone
        for (const std::string& f : settings_.recentFiles) {
            std::error_code ec;
            if (!fs::exists(f, ec))
                recent_.remove(f);
        }

    window_ = gtk_application_window_new(app_);
    g_object_ref(window_); // keep alive until our destructor
    gtk_window_set_default_size(GTK_WINDOW(window_), settings_.width, settings_.height);
    if (settings_.maximized)
        gtk_window_maximize(GTK_WINDOW(window_));
    gtk_window_set_icon_name(GTK_WINDOW(window_), NPP_APP_ID);
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
    g_signal_connect(window_, "key-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer self) -> gboolean {
                         auto* w = static_cast<MainWindow*>(self);
                         if (w->mruSwitching_ && (ev->keyval == GDK_KEY_Control_L || ev->keyval == GDK_KEY_Control_R)) {
                             w->mruSwitching_ = false;
                             w->refreshAll();
                         }
                         if (w->altAlone_ && w->settings_.hideMenu && (ev->keyval == GDK_KEY_Alt_L || ev->keyval == GDK_KEY_Alt_R))
                             gtk_widget_set_visible(w->menubar_, !gtk_widget_get_visible(w->menubar_));
                         w->altAlone_ = false;
                         return FALSE;
                     }), this);
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
    host.globallyModified = [this](EditorView& v) {
        for (const auto& t : tabs_)
            if (t->view.get() == &v) // upstream: hwndFrom is the buffer
                notifyPlugins(NPPN_GLOBALMODIFIED, t->buf.get(), 0);
    };
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
    applySettings();
    if (settings_.darkMode != 0)
        applyDarkMode();
    // Dark mode "follow": the desktop's colour scheme changes are followed live.
    if (GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) {
        colorSchemeBus_ = bus;
        colorSchemeWatch_ = g_dbus_connection_signal_subscribe(
            bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings", "SettingChanged", "/org/freedesktop/portal/desktop",
            "org.freedesktop.appearance", G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
            +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer self) {
                auto* w = static_cast<MainWindow*>(self);
                if (w->settings_.darkMode == 2 && !w->closing_)
                    w->applyDarkMode();
            },
            this, nullptr);
    }
}

MainWindow::~MainWindow()
{
    shutdownPlugins();
    closing_ = true;
    if (colorSchemeBus_) {
        g_dbus_connection_signal_unsubscribe(colorSchemeBus_, colorSchemeWatch_);
        g_object_unref(colorSchemeBus_);
    }
    if (tabCss_) {
        gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(tabCss_));
        g_object_unref(tabCss_);
    }
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
    plugins_.reset(); // their handles answer nothing from now on
    pluginButtons_.clear();
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
    const bool changed = darkTheme_ != dark;
    darkTheme_ = dark;
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, nullptr);
    if (changed && plugins_) {
        for (const auto& [cmd, item] : pluginButtons_)
            refreshPluginToolbarButton(cmd); // light or dark icon
        notifyPlugins(NPPN_DARKMODECHANGED);
    }
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
    refreshPluginMenu();

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
        if (!l.isPlainText() && !l.shortName.empty()
            && std::find(settings_.langHidden.begin(), settings_.langHidden.end(), l.name) == settings_.langHidden.end())
            byLetter[static_cast<char>(std::toupper(static_cast<unsigned char>(l.shortName[0])))].push_back(&l);
    if (!settings_.langMenuCompact) { // one flat list
        std::vector<const core::Language*> all;
        for (auto& [letter, list] : byLetter)
            all.insert(all.end(), list.begin(), list.end());
        std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return g_ascii_strcasecmp(a->shortName.c_str(), b->shortName.c_str()) < 0; });
        for (const core::Language* l : all)
            addTargetItem(dyn_.languages, escapeMnemonic(l->shortName), "win.language", l->name);
        byLetter.clear();
    }
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
    notifyPlugins(NPPN_NATIVELANGCHANGED);
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
            g_object_set_data(G_OBJECT(ti), "npp-icon", const_cast<char*>(it.icon)); // Preferences > Toolbar icon sets
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
    std::string name = b.name();
    // Preferences > Tab Bar: "Max. tab label length".
    if (settings_.tabMaxLabel > 0 && g_utf8_strlen(name.c_str(), -1) > settings_.tabMaxLabel) {
        gchar* cut = g_utf8_substring(name.c_str(), 0, settings_.tabMaxLabel);
        name = std::string(cut) + "…";
        g_free(cut);
    }
    const std::string label = (isModified(b) ? "*" : "") + name + (b.userReadOnly ? " [RO]" : "");
    if (t.closeButton)
        gtk_widget_set_visible(t.closeButton, settings_.tabCloseButton && (settings_.tabButtonsOnInactive || currentTabOf(t.viewIdx) == &t));
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
        title = (isModified(*b) ? "*" : "") + (settings_.titleFileNameOnly ? b->name() : b->fullName()) + " - Notepad++";
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
    if (!settings_.tabButtonsOnInactive)
        for (const auto& t : tabs_)
            if (t->closeButton)
                gtk_widget_set_visible(t->closeButton, settings_.tabCloseButton && currentTabOf(t->viewIdx) == t.get());
    if (const core::Buffer* b = activeBuffer(); b && !mruSwitching_ && (mru_.empty() || mru_.front() != b)) {
        mru_.erase(std::remove(mru_.begin(), mru_.end(), b), mru_.end());
        mru_.insert(mru_.begin(), b);
    }
    syncActionStates();
    refreshTitle();
    refreshStatus();
    refreshPanels();
    checkBufferActivated();
}

void MainWindow::refreshRecentMenu()
{
    if (!dyn_.recent)
        return;
    g_menu_remove_all(dyn_.recent);
    const auto& items = recent_.items();
    // Preferences > Recent Files History: in a submenu, and how names show.
    GMenu* target = settings_.recentSubmenu ? g_menu_new() : dyn_.recent;
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::string shown = items[i];
        if (settings_.recentDisplay == 0) {
            shown = fs::path(items[i]).filename().string();
        } else if (settings_.recentDisplay == 2 && g_utf8_strlen(shown.c_str(), -1) > settings_.recentMaxLen) {
            // Upstream PathCompactPathEx: the middle gives way to "...".
            const glong len = g_utf8_strlen(shown.c_str(), -1), keep = std::max(settings_.recentMaxLen - 3, 2);
            gchar* head = g_utf8_substring(shown.c_str(), 0, keep / 2);
            gchar* tail = g_utf8_substring(shown.c_str(), len - (keep - keep / 2), len);
            shown = std::string(head) + "..." + tail;
            g_free(head);
            g_free(tail);
        }
        // Upstream numbering: 1-9 get a mnemonic.
        const std::string num = i < 9 ? "_" + std::to_string(i + 1) : std::to_string(i + 1);
        addTargetItem(target, num + ": " + escapeMnemonic(shown), "win.open-recent", items[i]);
    }
    if (settings_.recentSubmenu) {
        if (!items.empty())
            g_menu_append_submenu(dyn_.recent, core::Translator::toMnemonic(tr_.subMenu("file-recentFiles", "&Recent Files")).c_str(),
                                  G_MENU_MODEL(target));
        g_object_unref(target);
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
    const platform::Settings& st = settings_;
    const core::Buffer* buf = nullptr;
    for (const auto& t : tabs_)
        if (t->view.get() == &v)
            buf = t->buf.get();
    const bool large = buf && buf->largeFile;
    v.execute(SCI_SETWRAPMODE, st.wordWrap && !(large && st.largeNoWrap) ? SC_WRAP_WORD : SC_WRAP_NONE);
    v.execute(SCI_SETVIEWWS, st.showSpaces ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
    v.execute(SCI_SETVIEWEOL, st.showEol);
    v.execute(SCI_SETINDENTATIONGUIDES, st.showIndentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
    v.execute(SCI_SETWRAPVISUALFLAGS, st.showWrapSymbol ? SC_WRAPVISUALFLAG_END : SC_WRAPVISUALFLAG_NONE);
    v.execute(SCI_SETZOOM, static_cast<uptr_t>(st.zoom));
    // Preferences > Indentation: the language's own settings, else the default.
    int tabSize = st.tabSize;
    bool spaces = st.replaceTabsBySpace;
    if (buf && buf->language)
        if (auto it = st.langIndent.find(buf->language->name); it != st.langIndent.end()) {
            int sp = spaces ? 1 : 0;
            if (std::sscanf(it->second.c_str(), "%d,%d", &tabSize, &sp) == 2)
                spaces = sp != 0;
        }
    v.execute(SCI_SETTABWIDTH, static_cast<uptr_t>(std::clamp(tabSize, 1, 64)));
    v.execute(SCI_SETUSETABS, !spaces);
    v.execute(SCI_SETBACKSPACEUNINDENTS, st.backspaceUnindent);
    v.autoIndent = st.autoIndentMode != 0;
    static const int kWrapIndent[] = {SC_WRAPINDENT_FIXED, SC_WRAPINDENT_SAME, SC_WRAPINDENT_INDENT};
    v.execute(SCI_SETWRAPINDENTMODE, static_cast<uptr_t>(kWrapIndent[std::clamp(st.wrapIndent, 0, 2)]));
    v.execute(SCI_SETCARETSTYLE, st.caretWidth == 0 ? CARETSTYLE_BLOCK : CARETSTYLE_LINE);
    v.execute(SCI_SETCARETWIDTH, static_cast<uptr_t>(std::max(st.caretWidth, 1)));
    v.execute(SCI_SETCARETPERIOD, static_cast<uptr_t>(st.caretBlinkMs));
    v.execute(SCI_SETENDATLASTLINE, !st.scrollBeyondLastLine);
    v.execute(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION | (st.virtualSpace ? SCVS_USERACCESSIBLE : 0));
    // Current line indicator: none, background, or a frame.
    v.execute(SCI_SETCARETLINEVISIBLE, st.currentLineMode != 0);
    v.execute(SCI_SETCARETLINEFRAME, st.currentLineMode == 2 ? static_cast<uptr_t>(st.currentLineFrame) : 0);
    v.execute(SCI_SETFONTQUALITY, st.smoothFont ? SC_EFF_QUALITY_LCD_OPTIMIZED : SC_EFF_QUALITY_DEFAULT);
    v.execute(SCI_SETDRAGDROPENABLED, !st.noSelDragDrop);
    const core::Style* sel = styles_.global("Selected text colour");
    if (st.selectedTextFg && sel && sel->fg) {
        v.execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_TEXT, static_cast<sptr_t>(*sel->fg | 0xFF000000u));
        v.execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_TEXT, static_cast<sptr_t>(*sel->fg | 0xFF000000u));
    } else {
        v.execute(SCI_RESETELEMENTCOLOUR, SC_ELEMENT_SELECTION_TEXT);
        v.execute(SCI_RESETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_TEXT);
    }
    v.execute(SCI_SETMULTIPLESELECTION, st.multiEditing);
    v.execute(SCI_SETADDITIONALSELECTIONTYPING, st.multiEditing || st.columnToMultiEdit);
    EditorView::Representations r;
    r.showNpc = st.showNpc;
    r.showCcUniEol = st.showCcUniEol;
    r.npcCodepoint = st.npcDisplay == 1;
    r.npcCustomColor = st.npcCustomColor;
    r.includeCcUniEol = st.npcIncludeCcUniEol;
    r.eolPlain = st.eolDisplay == 1;
    r.eolCustomColor = st.eolCustomColor;
    if (const core::Style* s = styles_.global("EOL custom color"); s && s->fg)
        r.eolColor = *s->fg;
    if (const core::Style* s = styles_.global("Non-printing characters custom color"); s && s->fg)
        r.npcColor = *s->fg;
    v.applyRepresentations(r);
    v.setFoldStyle(st.foldStyle);
    v.setLineNumbersVisible(st.showLineNumbers);
    v.refreshLineNumberWidth();
    v.setBookmarkMarginVisible(st.showBookmarkMargin);
    v.setChangeHistory(st.changeHistoryMargin, st.changeHistoryText);
    // Vertical edge: several columns in line mode (upstream multi-edge).
    std::vector<int> columns;
    {
        std::istringstream in(st.edgeColumns);
        int c = 0;
        while (in >> c)
            if (c > 0 && c <= 8192)
                columns.push_back(c);
    }
    v.execute(SCI_MULTIEDGECLEARALL);
    const core::Style* edge = styles_.global("Edge colour");
    const std::uint32_t edgeColour = edge && edge->fg ? *edge->fg : 0x80FFFF;
    if (st.edgeMode == 0 || columns.empty()) {
        v.execute(SCI_SETEDGEMODE, EDGE_NONE);
    } else if (st.edgeMode == 2) {
        v.execute(SCI_SETEDGEMODE, EDGE_BACKGROUND);
        v.execute(SCI_SETEDGECOLUMN, static_cast<uptr_t>(columns.front()));
        v.execute(SCI_SETEDGECOLOUR, edgeColour);
    } else if (columns.size() == 1) {
        v.execute(SCI_SETEDGEMODE, EDGE_LINE);
        v.execute(SCI_SETEDGECOLUMN, static_cast<uptr_t>(columns.front()));
        v.execute(SCI_SETEDGECOLOUR, edgeColour);
    } else {
        v.execute(SCI_SETEDGEMODE, EDGE_MULTILINE);
        for (int c : columns)
            v.execute(SCI_MULTIEDGEADDLINE, static_cast<uptr_t>(c), edgeColour);
    }
    v.execute(SCI_SETMARGINLEFT, 0, st.paddingLeft);
    v.execute(SCI_SETMARGINRIGHT, 0, st.paddingRight);
    // Preferences > Delimiter: extra word characters.
    if (st.extraWordCharsOn && !st.extraWordChars.empty()) {
        const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_" + st.extraWordChars;
        v.execute(SCI_SETWORDCHARS, 0, reinterpret_cast<sptr_t>(chars.c_str()));
    } else {
        v.execute(SCI_SETCHARSDEFAULT);
    }
    v.setLinkStyle(st.linkNoUnderline, st.linkFullBox);
    if (buf)
        languageExtras(v, *buf);
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
    w->shutdownPlugins(); // while their panels still exist
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

gboolean MainWindow::onKeyPress(GtkWidget* widget, GdkEventKey* ev, gpointer self)
{
    // Preferences > General > Menu "Hide": Alt or F10 shows it.
    auto* w = static_cast<MainWindow*>(self);
    w->altAlone_ = ev->keyval == GDK_KEY_Alt_L || ev->keyval == GDK_KEY_Alt_R;
    if (w->settings_.hideMenu && ev->keyval == GDK_KEY_F10 && !(ev->state & gtk_accelerator_get_default_mod_mask())) {
        const bool show = !gtk_widget_get_visible(w->menubar_);
        gtk_widget_set_visible(w->menubar_, show);
        if (show)
            gtk_menu_shell_select_first(GTK_MENU_SHELL(w->menubar_), FALSE);
        return TRUE;
    }
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
