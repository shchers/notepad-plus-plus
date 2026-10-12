#include "ui/MainWindow.h"

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

struct TabCtx {
    MainWindow* win;
    GtkWidget* page;
};

} // namespace

// ============================================================================
// Construction
// ============================================================================

MainWindow::MainWindow(GtkApplication* app, core::LanguageCatalog& langs) : app_(app), langs_(langs)
{
    settings_.load();

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

    notebook_ = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook_), TRUE);
    gtk_notebook_popup_enable(GTK_NOTEBOOK(notebook_));
    gtk_widget_set_vexpand(notebook_, TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), notebook_, TRUE, TRUE, 0);
    g_signal_connect(notebook_, "switch-page", G_CALLBACK(&MainWindow::onSwitchPage), this);

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

    gtk_widget_show_all(vbox);
}

MainWindow::~MainWindow()
{
    if (refreshIdle_)
        g_source_remove(refreshIdle_);
    settings_.save();
    docs_.clear(); // releases Scintilla widgets before the window goes
    if (window_) {
        g_signal_handlers_disconnect_by_data(window_, this);
        if (notebook_)
            g_signal_handlers_disconnect_by_data(notebook_, this);
        gtk_widget_destroy(window_);
        g_object_unref(window_);
    }
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
    commands_ = {
        {"new", [this] { newDocument(); }},
        {"open", [this] { cmdOpen(); }},
        {"reload", [this] {
             if (Document* d = activeDoc(); d && !d->isUntitled()) {
                 if (d->isModified() && !confirmClose(*d))
                     return;
                 std::string err;
                 if (!loadInto(*d, d->path, &err))
                     showError("Cannot reload file", err);
             }
         }},
        {"save", [this] { if (Document* d = activeDoc()) save(*d, false); }},
        {"save-as", [this] { if (Document* d = activeDoc()) save(*d, true); }},
        {"save-all", [this] {
             for (auto& d : docs_)
                 if (d->isModified() && !save(*d, false))
                     break;
         }},
        {"close", [this] { if (Document* d = activeDoc()) closeDocument(*d); }},
        {"close-all", [this] { cmdCloseAll(); }},
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

        {"find", [this] { findBar_->show(false); }},
        {"replace", [this] { findBar_->show(true); }},
        {"find-next", [this] { findBar_->findAgain(true); }},
        {"find-prev", [this] { findBar_->findAgain(false); }},
        {"goto", [this] { cmdGotoLine(); }},
        {"bookmark-toggle", [view] { if (EditorView* v = view()) v->toggleBookmarkAtCaret(); }},
        {"bookmark-next", [view] { if (EditorView* v = view()) v->gotoNextBookmark(true); }},
        {"bookmark-prev", [view] { if (EditorView* v = view()) v->gotoNextBookmark(false); }},

        {"zoom-in", [this] { sciCommand(SCI_ZOOMIN); if (EditorView* v = activeView()) { settings_.zoom = static_cast<int>(v->execute(SCI_GETZOOM)); for (auto& d : docs_) applyViewSettings(*d->view); } }},
        {"zoom-out", [this] { sciCommand(SCI_ZOOMOUT); if (EditorView* v = activeView()) { settings_.zoom = static_cast<int>(v->execute(SCI_GETZOOM)); for (auto& d : docs_) applyViewSettings(*d->view); } }},
        {"zoom-reset", [this] { settings_.zoom = 0; for (auto& d : docs_) applyViewSettings(*d->view); }},
        {"next-tab", [this] {
             const int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_));
             if (n > 1)
                 gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), (gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_)) + 1) % n);
         }},
        {"prev-tab", [this] {
             const int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_));
             if (n > 1)
                 gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), (gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_)) + n - 1) % n);
         }},
        {"about", [this] { cmdAbout(); }},
    };
    for (const auto& [name, fn] : commands_) {
        GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(&MainWindow::onAction), this);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(a));
        g_object_unref(a);
    }

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
    addItem(s, "Re_load from Disk", "win.reload");
    s = section(file);
    addItem(s, "_Save", "win.save");
    addItem(s, "Save _As…", "win.save-as");
    addItem(s, "Sa_ve All", "win.save-all");
    s = section(file);
    addItem(s, "_Close", "win.close");
    addItem(s, "Clos_e All", "win.close-all");
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

    GMenu* help = submenu(bar, "_?");
    addItem(help, "_About Notepad++ (GTK)…", "win.about");

    GtkWidget* menubar = gtk_menu_bar_new_from_model(G_MENU_MODEL(bar));
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    g_object_unref(bar);
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
// Documents
// ============================================================================

MainWindow::Document* MainWindow::addDocument(std::unique_ptr<Document> doc)
{
    Document* d = doc.get();
    EditorView& v = *d->view;
    applyViewSettings(v);

    GtkWidget* page = v.widget();
    v.onUpdateUI = [this, d] {
        if (d == activeDoc())
            refreshStatus();
    };
    v.onSavePoint = [this, d](bool) {
        refreshTab(*d);
        if (d == activeDoc())
            refreshTitle();
    };
    v.onUriDropped = [this](std::string_view uris) { openUriList(uris); };

    // Tab label: [name][x], middle click closes (upstream option, on by default here).
    GtkWidget* ebox = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ebox), FALSE);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    d->tabText = gtk_label_new(nullptr);
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close, FALSE);
    gtk_widget_set_tooltip_text(close, "Close");
    gtk_box_pack_start(GTK_BOX(box), d->tabText, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ebox), box);
    gtk_widget_show_all(ebox);
    d->tabLabel = ebox;

    auto freeCtx = +[](gpointer p, GClosure*) { delete static_cast<TabCtx*>(p); };
    g_signal_connect_data(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                              auto* c = static_cast<TabCtx*>(p);
                              if (Document* doc = c->win->docForPage(c->page))
                                  c->win->closeDocument(*doc);
                          }),
                          new TabCtx{this, page}, freeCtx, GConnectFlags(0));
    g_signal_connect_data(ebox, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer p) -> gboolean {
                              auto* c = static_cast<TabCtx*>(p);
                              if (ev->type == GDK_BUTTON_PRESS && ev->button == 2) {
                                  if (Document* doc = c->win->docForPage(c->page))
                                      c->win->closeDocument(*doc);
                                  return TRUE;
                              }
                              return FALSE;
                          }),
                          new TabCtx{this, page}, freeCtx, GConnectFlags(0));

    docs_.push_back(std::move(doc));
    gtk_widget_show(page);
    const int idx = gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), page, ebox);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook_), page, TRUE);
    refreshTab(*d);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), idx);
    gtk_widget_grab_focus(page);
    return d;
}

std::string MainWindow::nextUntitledName() const
{
    for (int n = 1;; ++n) {
        std::string name = "new " + std::to_string(n);
        if (std::none_of(docs_.begin(), docs_.end(), [&](const auto& d) { return d->isUntitled() && d->untitledName == name; }))
            return name;
    }
}

void MainWindow::newDocument()
{
    auto doc = std::make_unique<Document>();
    doc->view = std::make_unique<EditorView>();
    doc->untitledName = nextUntitledName();
    Document* d = addDocument(std::move(doc));
    setLanguage(*d, langs_.plainText());
    syncActionStates();
    refreshTitle();
    refreshStatus();
}

MainWindow::Document* MainWindow::activeDoc() const
{
    const int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_));
    if (page < 0)
        return nullptr;
    return docForPage(gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_), page));
}

EditorView* MainWindow::activeView() const
{
    Document* d = activeDoc();
    return d ? d->view.get() : nullptr;
}

std::string MainWindow::activeTitle() const
{
    return gtk_window_get_title(GTK_WINDOW(window_)) ? gtk_window_get_title(GTK_WINDOW(window_)) : "";
}

MainWindow::Document* MainWindow::docForPage(GtkWidget* page) const
{
    for (const auto& d : docs_)
        if (d->view->widget() == page)
            return d.get();
    return nullptr;
}

MainWindow::Document* MainWindow::findByPath(const fs::path& p) const
{
    for (const auto& d : docs_)
        if (!d->isUntitled() && d->path == p)
            return d.get();
    return nullptr;
}

bool MainWindow::loadInto(Document& doc, const fs::path& p, std::string* error)
{
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        *error = p.string() + " is a directory.";
        return false;
    }
    std::string raw;
    if (fs::exists(p, ec)) {
        gchar* contents = nullptr;
        gsize len = 0;
        GError* err = nullptr;
        if (!g_file_get_contents(p.c_str(), &contents, &len, &err)) {
            *error = err ? err->message : "read failed";
            if (err)
                g_error_free(err);
            return false;
        }
        raw.assign(contents, len);
        g_free(contents);
    }
    // A path that does not exist yet opens as an empty document that will be
    // created on save (upstream asks first; good enough for the skeleton).
    platform::DecodedText dec = platform::decode(raw);
    doc.path = p;
    doc.encoding = dec.encoding;
    doc.encodingChanged = false;
    doc.view->execute(SCI_SETREADONLY, 0);
    doc.view->setText(dec.utf8);
    doc.view->setEolMode(doc.view->detectEolMode(SC_EOL_LF), false);
    setLanguage(doc, langs_.forFile(p));
    lastDir_ = p.parent_path();
    return true;
}

int MainWindow::openFiles(const std::vector<fs::path>& files, const OpenOptions& opts)
{
    int opened = 0;
    for (const fs::path& f : files) {
        std::error_code ec;
        fs::path p = fs::weakly_canonical(fs::absolute(f, ec), ec);
        if (ec)
            p = fs::absolute(f);

        Document* d = findByPath(p);
        if (!d) {
            // Replace a lone pristine "new 1", as upstream does.
            Document* reuse = nullptr;
            if (docs_.size() == 1 && docs_[0]->isUntitled() && !docs_[0]->isModified() && docs_[0]->view->length() == 0)
                reuse = docs_[0].get();

            std::unique_ptr<Document> fresh;
            Document* target = reuse;
            if (!target) {
                fresh = std::make_unique<Document>();
                fresh->view = std::make_unique<EditorView>();
                target = fresh.get();
            }
            std::string err;
            if (!loadInto(*target, p, &err)) {
                showError("Cannot open file \"" + p.filename().string() + "\"", err);
                continue;
            }
            d = fresh ? addDocument(std::move(fresh)) : target;
            refreshTab(*d);
            ++opened;
        }
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_), d->view->widget()));

        if (opts.language)
            setLanguage(*d, *opts.language);
        if (opts.line || opts.column)
            d->view->gotoLine(opts.line.value_or(1) - 1, opts.column.value_or(1) - 1);
        if (opts.readOnly)
            d->view->execute(SCI_SETREADONLY, 1);
    }
    syncActionStates();
    refreshTitle();
    refreshStatus();
    return opened;
}

void MainWindow::setLanguage(Document& doc, const core::Language& lang)
{
    doc.view->applyLanguage(lang, langs_, styles_);
    applyViewSettings(*doc.view);
    if (&doc == activeDoc()) {
        syncActionStates();
        refreshStatus();
    }
}

bool MainWindow::save(Document& doc, bool saveAs)
{
    fs::path target = doc.path;
    if (doc.isUntitled() || saveAs) {
        GtkFileChooserNative* dlg = gtk_file_chooser_native_new("Save As", GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        if (!doc.isUntitled())
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), doc.path.c_str());
        else {
            if (!lastDir_.empty())
                gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), lastDir_.c_str());
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), doc.untitledName.c_str());
        }
        const gint res = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
        if (res == GTK_RESPONSE_ACCEPT) {
            gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            if (fn) {
                target = fn;
                g_free(fn);
            }
        }
        g_object_unref(dlg);
        if (res != GTK_RESPONSE_ACCEPT || target.empty())
            return false;
    }

    std::string err;
    auto bytes = platform::encode(doc.view->text(), doc.encoding, &err);
    if (!bytes) {
        showError("Cannot save \"" + doc.name() + "\" as " + doc.encoding.displayName(),
                  "The text contains characters this encoding cannot represent (" + err +
                      "). Choose Encoding > UTF-8 and save again.");
        return false;
    }
    GFile* gf = g_file_new_for_path(target.c_str());
    GError* gerr = nullptr;
    const gboolean ok = g_file_replace_contents(gf, bytes->data(), bytes->size(), nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, nullptr, &gerr);
    g_object_unref(gf);
    if (!ok) {
        showError("Cannot save \"" + target.filename().string() + "\"", gerr ? gerr->message : "write failed");
        if (gerr)
            g_error_free(gerr);
        return false;
    }

    const bool wasUntitled = doc.isUntitled();
    const bool nameChanged = doc.path != target;
    doc.path = target;
    lastDir_ = target.parent_path();
    doc.encodingChanged = false;
    doc.view->execute(SCI_SETSAVEPOINT);
    // Upstream re-detects the language when an untitled/plain doc gets a name.
    if ((wasUntitled || nameChanged) && (!doc.view->language() || doc.view->language()->isPlainText()))
        setLanguage(doc, langs_.forFile(target));
    refreshTab(doc);
    refreshTitle();
    refreshStatus();
    return true;
}

bool MainWindow::confirmClose(Document& doc)
{
    if (!doc.isModified())
        return true;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), gtk_notebook_page_num(GTK_NOTEBOOK(notebook_), doc.view->widget()));
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                            "Save file \"%s\"?", doc.isUntitled() ? doc.untitledName.c_str() : doc.path.c_str());
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), "_Cancel", GTK_RESPONSE_CANCEL, "_No", GTK_RESPONSE_NO, "_Yes", GTK_RESPONSE_YES, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
    const gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r == GTK_RESPONSE_YES)
        return save(doc, false);
    return r == GTK_RESPONSE_NO;
}

bool MainWindow::closeDocument(Document& doc, bool keepOne)
{
    if (!confirmClose(doc))
        return false;
    const int page = gtk_notebook_page_num(GTK_NOTEBOOK(notebook_), doc.view->widget());
    auto it = std::find_if(docs_.begin(), docs_.end(), [&](const auto& p) { return p.get() == &doc; });
    std::unique_ptr<Document> owned = std::move(*it); // keep alive while GTK detaches it
    docs_.erase(it);
    if (page >= 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_), page);
    owned.reset();
    if (docs_.empty() && keepOne)
        newDocument(); // upstream always keeps one document open
    refreshTitle();
    refreshStatus();
    return true;
}

bool MainWindow::cmdCloseAll()
{
    while (!docs_.empty())
        if (!closeDocument(*docs_.back(), false))
            return false;
    newDocument();
    return true;
}

// ============================================================================
// UI refresh
// ============================================================================

void MainWindow::refreshTab(Document& doc)
{
    const std::string label = (doc.isModified() ? "*" : "") + doc.name();
    gtk_label_set_text(GTK_LABEL(doc.tabText), label.c_str());
    gtk_widget_set_tooltip_text(doc.tabLabel, doc.isUntitled() ? doc.untitledName.c_str() : doc.path.c_str());
    gtk_notebook_set_menu_label_text(GTK_NOTEBOOK(notebook_), doc.view->widget(), doc.name().c_str());
}

void MainWindow::refreshTitle()
{
    std::string title = "Notepad++";
    if (Document* d = activeDoc())
        title = (d->isModified() ? "*" : "") + (d->isUntitled() ? d->untitledName : d->path.string()) + " - Notepad++";
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
}

void MainWindow::refreshStatus()
{
    Document* d = activeDoc();
    if (!d) {
        for (GtkWidget* l : {stLang_, stStats_, stPos_, stEol_, stEnc_, stIns_})
            gtk_label_set_text(GTK_LABEL(l), "");
        return;
    }
    EditorView& v = *d->view;
    const core::Language* lang = v.language();
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
    gtk_label_set_text(GTK_LABEL(stEnc_), d->encoding.displayName().c_str());
    gtk_label_set_text(GTK_LABEL(stIns_), v.execute(SCI_GETOVERTYPE) ? "OVR" : "INS");
}

void MainWindow::syncActionStates()
{
    Document* d = activeDoc();
    if (!d)
        return;
    syncingActions_ = true;
    auto set = [this](const char* name, GVariant* v) {
        if (GAction* a = g_action_map_lookup_action(G_ACTION_MAP(window_), name))
            g_simple_action_set_state(G_SIMPLE_ACTION(a), v);
        else
            g_variant_unref(g_variant_ref_sink(v));
    };
    const core::Language* lang = d->view->language();
    set("language", g_variant_new_string(lang ? lang->name.c_str() : "normal"));
    set("encoding", g_variant_new_string(encodingId(d->encoding).c_str()));
    set("eol", g_variant_new_string(eolId(static_cast<int>(d->view->execute(SCI_GETEOLMODE))).c_str()));
    syncingActions_ = false;
}

void MainWindow::applyViewSettings(EditorView& v)
{
    v.execute(SCI_SETWRAPMODE, settings_.wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE);
    v.execute(SCI_SETVIEWWS, settings_.showAllChars ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
    v.execute(SCI_SETVIEWEOL, settings_.showAllChars);
    v.execute(SCI_SETINDENTATIONGUIDES, settings_.showIndentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE);
    v.execute(SCI_SETZOOM, static_cast<uptr_t>(settings_.zoom));
}

void MainWindow::showError(const std::string& primary, const std::string& secondary)
{
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", primary.c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", secondary.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
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
    if (Document* d = activeDoc(); d && !d->isUntitled())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), d->path.parent_path().c_str());
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
    for (auto& d : docs_)
        if (const core::Language* l = d->view->language())
            setLanguage(*d, *l);
}

void MainWindow::cmdSetEncoding(const std::string& id)
{
    Document* d = activeDoc();
    if (!d)
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
    if (e == d->encoding)
        return;
    // Text is held as UTF-8, so this is upstream's "Convert to ...": the bytes
    // on disk change on the next save.
    d->encoding = e;
    d->encodingChanged = true;
    refreshTab(*d);
    refreshTitle();
    refreshStatus();
}

void MainWindow::cmdSetEol(const std::string& id)
{
    Document* d = activeDoc();
    if (!d)
        return;
    const int mode = id == "crlf" ? SC_EOL_CRLF : id == "cr" ? SC_EOL_CR : SC_EOL_LF;
    d->view->setEolMode(mode, true);
    refreshStatus();
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
    if (name == "word-wrap" || name == "show-all" || name == "indent-guides") {
        const bool on = g_variant_get_boolean(value);
        (name == "word-wrap" ? w->settings_.wordWrap : name == "show-all" ? w->settings_.showAllChars : w->settings_.showIndentGuides) = on;
        for (auto& d : w->docs_)
            w->applyViewSettings(*d->view);
        return;
    }
    const std::string s = g_variant_get_string(value, nullptr);
    if (name == "language") {
        if (Document* d = w->activeDoc())
            if (const core::Language* l = w->langs_.byName(s))
                w->setLanguage(*d, *l);
    } else if (name == "encoding") {
        w->cmdSetEncoding(s);
    } else if (name == "eol") {
        w->cmdSetEol(s);
    } else if (name == "theme") {
        w->cmdSetTheme(s);
    }
}

void MainWindow::onSwitchPage(GtkNotebook*, GtkWidget* page, guint, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    // Called during switch: current page is not updated yet, so resolve by widget.
    Document* d = w->docForPage(page);
    if (!d)
        return;
    gtk_widget_grab_focus(page);
    // Defer refresh until GtkNotebook has updated its current page.
    if (!w->refreshIdle_)
        w->refreshIdle_ = g_idle_add(+[](gpointer p) -> gboolean {
            auto* win = static_cast<MainWindow*>(p);
            win->refreshIdle_ = 0;
            win->syncActionStates();
            win->refreshTitle();
            win->refreshStatus();
            return G_SOURCE_REMOVE;
        }, w);
}

gboolean MainWindow::onDeleteEvent(GtkWidget*, GdkEvent*, gpointer self)
{
    auto* w = static_cast<MainWindow*>(self);
    for (auto& d : w->docs_)
        if (!w->confirmClose(*d))
            return TRUE; // cancelled: keep window open
    w->settings_.save();
    return FALSE;
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
