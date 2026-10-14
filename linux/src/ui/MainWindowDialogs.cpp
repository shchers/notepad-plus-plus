// Settings dialogs that edit MainWindow state directly: the Shortcut Mapper
// (upstream ShortcutMapper.cpp).

#include "ui/MainWindow.h"

#include "platform/Paths.h"
#include "core/EditOps.h"
#include "ui/ShortcutDialog.h"

#include "menuCmdID.h"
#include <windows.h> // plugin SDK: NPPN_* codes
#include "Notepad_plus_msgs.h"

#include <algorithm>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

enum Col { kColName, kColKey, kColCategory, kColId, kColNth, kColCount };
enum MapperTab { kTabMenu = 0, kTabMacros, kTabRun, kTabPlugins, kTabCount };

std::string lower(std::string s)
{
    gchar* l = g_utf8_strdown(s.c_str(), -1);
    s = l;
    g_free(l);
    return s;
}

} // namespace

// The Shortcut Mapper works on copies and writes shortcuts.xml on each change,
// as upstream does.
class ShortcutMapper {
public:
    ShortcutMapper(MainWindow& w, core::CommandKeys& keys, core::ShortcutsFile& file, const core::Translator& tr, const CommandTable& cmds)
        : w_(w), keys_(keys), file_(file), tr_(tr), cmds_(cmds), infos_(commandInfos(tr))
    {
    }

    void run(GtkWindow* parent, int tab);

private:
    std::string t(const char* key, const char* fallback) const { return tr_.dialogAttr("ShortcutMapper/" + std::string(key), "name", fallback); }
    std::string commandName(int id) const
    {
        if (auto it = infos_.find(id); it != infos_.end())
            return it->second.name;
        if (const char* n = core::CommandKeys::defaultName(id))
            return n;
        return "Command " + std::to_string(id);
    }
    void fill();
    std::string conflicts(const core::KeyCombo& k, int tab, int id, int nth) const;
    bool selected(int& id, int& nth) const;
    void modify();
    void clear();
    void remove();
    void updateInfo();
    void save();

    MainWindow& w_;
    core::CommandKeys& keys_;
    core::ShortcutsFile& file_;
    const core::Translator& tr_;
    const CommandTable& cmds_;
    std::map<int, CommandInfo> infos_;
    GtkWidget* dlg_ = nullptr;
    GtkWidget* tabs_ = nullptr;
    GtkWidget* views_[kTabCount] = {};
    GtkListStore* stores_[kTabCount] = {};
    GtkTreeModel* filters_[kTabCount] = {};
    std::vector<MainWindow::PluginCommandInfo> plugins_; // refreshed by fill()
    const MainWindow::PluginCommandInfo* plugin(int cmdId) const
    {
        for (const auto& p : plugins_)
            if (p.cmdId == cmdId)
                return &p;
        return nullptr;
    }
    GtkWidget* filter_ = nullptr;
    GtkWidget* info_ = nullptr;
    GtkWidget* deleteBtn_ = nullptr;
    int tab() const { return gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs_)); }
};

void ShortcutMapper::fill()
{
    for (GtkListStore* s : stores_)
        gtk_list_store_clear(s);
    for (const core::CommandKey& k : keys_.all()) {
        if (!cmds_.has(k.id))
            continue; // a command the port does not have
        const auto it = infos_.find(k.id);
        gtk_list_store_insert_with_values(stores_[kTabMenu], nullptr, -1, kColName, commandName(k.id).c_str(), kColKey,
                                          k.key.toString().c_str(), kColCategory, it != infos_.end() ? it->second.category.c_str() : "",
                                          kColId, k.id, kColNth, k.nth, -1);
    }
    for (std::size_t i = 0; i < file_.macros.size(); ++i)
        gtk_list_store_insert_with_values(stores_[kTabMacros], nullptr, -1, kColName, file_.macros[i].name.c_str(), kColKey,
                                          file_.macros[i].key.toString().c_str(), kColCategory, "", kColId, static_cast<int>(i), kColNth, 0, -1);
    for (std::size_t i = 0; i < file_.userCommands.size(); ++i)
        gtk_list_store_insert_with_values(stores_[kTabRun], nullptr, -1, kColName, file_.userCommands[i].name.c_str(), kColKey,
                                          file_.userCommands[i].key.toString().c_str(), kColCategory, "", kColId, static_cast<int>(i), kColNth, 0, -1);
    plugins_ = w_.pluginCommandList();
    for (const auto& p : plugins_)
        gtk_list_store_insert_with_values(stores_[kTabPlugins], nullptr, -1, kColName, core::Translator::plain(p.name).c_str(), kColKey,
                                          p.key.toString().c_str(), kColCategory, core::Translator::plain(p.plugin).c_str(), kColId, p.cmdId,
                                          kColNth, 0, -1);
}

std::string ShortcutMapper::conflicts(const core::KeyCombo& k, int tab, int id, int nth) const
{
    if (k.empty())
        return {};
    std::string out;
    auto add = [&](const std::string& where, const std::string& name) {
        out += (out.empty() ? "" : "\n") + where + "  |  " + name;
    };
    const std::string menuTab = t("MainMenuTab", "Main menu"), macroTab = t("MacrosTab", "Macros"), runTab = t("RunCommandsTab", "Run commands");
    const std::string pluginTab = t("PluginCommandsTab", "Plugin commands");
    for (const core::CommandKey& c : keys_.conflicts(k, tab == kTabMenu ? id : 0, tab == kTabMenu ? nth : -1))
        if (cmds_.has(c.id))
            add(menuTab, commandName(c.id));
    for (std::size_t i = 0; i < file_.macros.size(); ++i)
        if (file_.macros[i].key == k && !(tab == kTabMacros && static_cast<int>(i) == id))
            add(macroTab, file_.macros[i].name);
    for (std::size_t i = 0; i < file_.userCommands.size(); ++i)
        if (file_.userCommands[i].key == k && !(tab == kTabRun && static_cast<int>(i) == id))
            add(runTab, file_.userCommands[i].name);
    for (const auto& p : plugins_)
        if (p.key == k && !(tab == kTabPlugins && p.cmdId == id))
            add(pluginTab, core::Translator::plain(p.name));
    return out;
}

bool ShortcutMapper::selected(int& id, int& nth) const
{
    GtkTreeModel* model = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(views_[tab()])), &model, &it))
        return false;
    gtk_tree_model_get(model, &it, kColId, &id, kColNth, &nth, -1);
    return true;
}

void ShortcutMapper::updateInfo()
{
    int id = 0, nth = 0;
    gtk_widget_set_sensitive(deleteBtn_, tab() == kTabMacros || tab() == kTabRun);
    if (!selected(id, nth)) {
        gtk_label_set_text(GTK_LABEL(info_), "");
        return;
    }
    core::KeyCombo k;
    if (tab() == kTabMenu) {
        for (const core::CommandKey& c : keys_.all())
            if (c.id == id && c.nth == nth)
                k = c.key;
    } else if (tab() == kTabMacros) {
        k = file_.macros[static_cast<std::size_t>(id)].key;
    } else if (tab() == kTabPlugins) {
        if (const auto* p = plugin(id))
            k = p->key;
    } else {
        k = file_.userCommands[static_cast<std::size_t>(id)].key;
    }
    const std::string c = conflicts(k, tab(), id, nth);
    gtk_label_set_text(GTK_LABEL(info_), c.empty() ? tr_.dialogAttr("ShortcutMapper/ConflictInfoOk", "name", "No shortcut conflicts for this item.").c_str()
                                                   : c.c_str());
}

void ShortcutMapper::save()
{
    std::string err;
    if (!w_.saveShortcuts(&err))
        g_warning("Cannot save shortcuts.xml: %s", err.c_str());
    w_.reloadShortcuts(); // menus and accelerators from what was written
    fill();
}

void ShortcutMapper::modify()
{
    int id = 0, nth = 0;
    if (!selected(id, nth))
        return;
    const int tb = tab();
    ShortcutEdit e;
    if (tb == kTabMenu) {
        e.name = commandName(id);
        e.nameEditable = false;
        for (const core::CommandKey& c : keys_.all())
            if (c.id == id && c.nth == nth)
                e.key = c.key;
    } else if (tb == kTabMacros) {
        e.name = file_.macros[static_cast<std::size_t>(id)].name;
        e.key = file_.macros[static_cast<std::size_t>(id)].key;
    } else if (tb == kTabPlugins) {
        const auto* p = plugin(id);
        if (!p)
            return;
        e.name = core::Translator::plain(p->name);
        e.nameEditable = false;
        e.key = p->key;
    } else {
        e.name = file_.userCommands[static_cast<std::size_t>(id)].name;
        e.key = file_.userCommands[static_cast<std::size_t>(id)].key;
    }
    e.conflictInfo = [this, tb, id, nth](const core::KeyCombo& k) {
        const std::string c = conflicts(k, tb, id, nth);
        return c.empty() ? std::string() : tr_.dialog("ShortcutMapperSubDialg", 5012, "CONFLICT FOUND!") + "\n" + c;
    };
    if (!runShortcutDialog(GTK_WINDOW(dlg_), tr_, e))
        return;
    if (tb == kTabMenu) {
        keys_.set(id, nth, e.key);
    } else if (tb == kTabPlugins) {
        const auto* p = plugin(id);
        file_.setPluginKey(p->module, p->internalId, e.key);
        save();
        w_.notifyShortcutRemapped(id);
        return;
    } else if (tb == kTabMacros) {
        file_.macros[static_cast<std::size_t>(id)].name = e.name;
        file_.macros[static_cast<std::size_t>(id)].key = e.key;
    } else {
        file_.userCommands[static_cast<std::size_t>(id)].name = e.name;
        file_.userCommands[static_cast<std::size_t>(id)].key = e.key;
    }
    save();
}

void ShortcutMapper::clear()
{
    int id = 0, nth = 0;
    if (!selected(id, nth))
        return;
    if (tab() == kTabMenu) {
        keys_.set(id, nth, {});
    } else if (tab() == kTabPlugins) {
        if (const auto* p = plugin(id)) {
            file_.setPluginKey(p->module, p->internalId, {});
            save();
            w_.notifyShortcutRemapped(id);
        }
        return;
    } else if (tab() == kTabMacros)
        file_.macros[static_cast<std::size_t>(id)].key = {};
    else
        file_.userCommands[static_cast<std::size_t>(id)].key = {};
    save();
}

void ShortcutMapper::remove()
{
    int id = 0, nth = 0;
    if (!selected(id, nth) || (tab() != kTabMacros && tab() != kTabRun))
        return;
    if (tab() == kTabMacros)
        file_.macros.erase(file_.macros.begin() + id);
    else
        file_.userCommands.erase(file_.userCommands.begin() + id);
    save();
}

void ShortcutMapper::run(GtkWindow* parent, int initialTab)
{
    dlg_ = gtk_dialog_new_with_buttons(tr_.dialogAttr("ShortcutMapper", "title", "Shortcut Mapper").c_str(), parent, GTK_DIALOG_MODAL,
                                       core::Translator::toMnemonic(tr_.dialog("ShortcutMapper", 1, "Close")).c_str(), GTK_RESPONSE_CLOSE,
                                       nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg_), 760, 560);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg_));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 6);

    filter_ = gtk_search_entry_new();
    tabs_ = gtk_notebook_new();
    const std::string tabNames[] = {t("MainMenuTab", "Main menu"), t("MacrosTab", "Macros"), t("RunCommandsTab", "Run commands"),
                                    t("PluginCommandsTab", "Plugin commands")};
    for (int i = 0; i < kTabCount; ++i) {
        stores_[i] = gtk_list_store_new(kColCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
        filters_[i] = gtk_tree_model_filter_new(GTK_TREE_MODEL(stores_[i]), nullptr);
        GtkWidget* view = gtk_tree_view_new_with_model(filters_[i]);
        views_[i] = view;
        const std::string cols[] = {t("ColumnName", "Name"), t("ColumnShortcut", "Shortcut"), t("ColumnCategory", "Category")};
        for (int c = 0; c < (i == kTabMenu || i == kTabPlugins ? 3 : 2); ++c) {
            GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(cols[c].c_str(), gtk_cell_renderer_text_new(), "text", c, nullptr);
            gtk_tree_view_column_set_resizable(col, TRUE);
            gtk_tree_view_column_set_sort_column_id(col, c);
            if (c == 0)
                gtk_tree_view_column_set_expand(col, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
        }
        g_signal_connect_swapped(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), "changed",
                                 G_CALLBACK(+[](ShortcutMapper* m) { m->updateInfo(); }), this);
        g_signal_connect(view, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*, gpointer p) {
                             static_cast<ShortcutMapper*>(p)->modify();
                         }), this);
        GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_container_add(GTK_CONTAINER(sw), view);
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs_), sw, gtk_label_new(tabNames[i].c_str()));
    }
    gtk_box_pack_start(GTK_BOX(box), tabs_, TRUE, TRUE, 0);

    info_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(info_), 0);
    gtk_label_set_selectable(GTK_LABEL(info_), TRUE);
    gtk_box_pack_start(GTK_BOX(box), info_, FALSE, FALSE, 0);

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    auto button = [&](int id, const char* fb, void (*fn)(ShortcutMapper*)) {
        GtkWidget* b = gtk_button_new_with_mnemonic(core::Translator::toMnemonic(tr_.dialog("ShortcutMapper", id, fb)).c_str());
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(fn), this);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
        return b;
    };
    button(2602, "Modify", +[](ShortcutMapper* m) { m->modify(); });
    button(2606, "Clear", +[](ShortcutMapper* m) { m->clear(); });
    deleteBtn_ = button(2603, "Delete", +[](ShortcutMapper* m) { m->remove(); });
    gtk_box_pack_end(GTK_BOX(row), filter_, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), gtk_label_new(core::Translator::plain(tr_.dialog("ShortcutMapper", 2607, "Filter: ")).c_str()), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    for (GtkTreeModel* f : filters_)
        gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(f), +[](GtkTreeModel* m, GtkTreeIter* it, gpointer p) -> gboolean {
            const std::string fl = lower(gtk_entry_get_text(GTK_ENTRY(p)));
            if (fl.empty())
                return TRUE;
            gchar *name = nullptr, *key = nullptr, *cat = nullptr;
            gtk_tree_model_get(m, it, kColName, &name, kColKey, &key, kColCategory, &cat, -1);
            const std::string all = lower(std::string(name ? name : "") + "\n" + (key ? key : "") + "\n" + (cat ? cat : ""));
            g_free(name);
            g_free(key);
            g_free(cat);
            return all.find(fl) != std::string::npos;
        }, filter_, nullptr);
    g_signal_connect_swapped(filter_, "search-changed", G_CALLBACK(+[](ShortcutMapper* m) {
                                 for (GtkTreeModel* f : m->filters_)
                                     gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(f));
                             }), this);
    g_signal_connect_swapped(tabs_, "switch-page", G_CALLBACK(+[](ShortcutMapper* m) {
                                 g_idle_add(+[](gpointer p) -> gboolean { static_cast<ShortcutMapper*>(p)->updateInfo(); return G_SOURCE_REMOVE; }, m);
                             }), this);

    fill();
    gtk_widget_show_all(box);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs_), initialTab);
    gtk_dialog_run(GTK_DIALOG(dlg_));
    while (g_main_context_iteration(nullptr, FALSE)) { // the pending idle above uses `this`
    }
    gtk_widget_destroy(dlg_);
    for (int i = 0; i < kTabCount; ++i) {
        g_object_unref(filters_[i]);
        g_object_unref(stores_[i]);
    }
}

void MainWindow::cmdShortcutMapper(int tab)
{
    ShortcutMapper m(*this, keys_, shortcuts_, tr_, cmds_);
    m.run(GTK_WINDOW(window_), tab);
}

} // namespace npp::ui

// ============================================================================
// Style Configurator (upstream WordStyleDlg)
// ============================================================================

namespace npp::ui {

void MainWindow::restyleAll()
{
    for (core::Buffer* b : buffers())
        reapplyLanguage(*b);
    results_->applyStyles(styles_);
    terminal_->applyStyles(styles_);
    panelSource_ = nullptr; // the Document Map takes the new styles
    refreshPanels();
    notifyPlugins(NPPN_WORDSTYLESUPDATED, activeBuffer());
}

void MainWindow::cmdStyleConfigurator()
{
    const char* sec = "StyleConfig";
    const std::string sub = "StyleConfig/SubDialog";
    auto text = [&](int id, const char* fb) { return core::Translator::plain(tr_.dialog(sub, id, fb)); };
    std::string theme = settings_.theme;

    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr(sec, "title", "Style Configurator").c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                                 core::Translator::toMnemonic(tr_.dialog(sec, 2, "Cancel")).c_str(), GTK_RESPONSE_CANCEL,
                                                 core::Translator::toMnemonic(tr_.dialog(sec, 2301, "Save && Close")).c_str(), GTK_RESPONSE_OK,
                                                 nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 820, 560);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    // Theme
    GtkWidget* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(top), gtk_label_new(core::Translator::plain(tr_.dialog(sec, 2306, "Select theme: ")).c_str()), FALSE, FALSE, 0);
    GtkWidget* themes = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(themes), "", "Default (stylers.xml)");
    for (const std::string& t : availableThemes())
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(themes), t.c_str(), fs::path(t).stem().c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(themes), theme.c_str());
    gtk_box_pack_start(GTK_BOX(top), themes, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), top, FALSE, FALSE, 0);

    GtkWidget* main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content), main, TRUE, TRUE, 0);
    auto listColumn = [&](const std::string& title, GtkListStore*& store, GtkWidget*& view) {
        GtkWidget* col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget* l = gtk_label_new(title.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_pack_start(GTK_BOX(col), l, FALSE, FALSE, 0);
        store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING); // label, key
        view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        g_object_unref(store);
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", 0, nullptr));
        GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(sw, 190, -1);
        gtk_container_add(GTK_CONTAINER(sw), view);
        gtk_box_pack_start(GTK_BOX(col), sw, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(main), col, FALSE, FALSE, 0);
    };
    GtkListStore *langStore = nullptr, *styleStore = nullptr;
    GtkWidget *langView = nullptr, *styleView = nullptr;
    listColumn(text(2225, "Language:"), langStore, langView);
    listColumn(text(2211, "Style:"), styleStore, styleView);

    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(main), right, TRUE, TRUE, 0);
    GtkWidget* colorFrame = gtk_frame_new(text(2212, "Color Style").c_str());
    GtkWidget* cg = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cg), 6);
    gtk_grid_set_column_spacing(GTK_GRID(cg), 8);
    gtk_container_set_border_width(GTK_CONTAINER(cg), 8);
    gtk_container_add(GTK_CONTAINER(colorFrame), cg);
    GtkWidget* fgBtn = gtk_color_button_new();
    GtkWidget* bgBtn = gtk_color_button_new();
    gtk_grid_attach(GTK_GRID(cg), gtk_label_new(text(2206, "Foreground color").c_str()), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cg), fgBtn, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cg), gtk_label_new(text(2207, "Background color").c_str()), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cg), bgBtn, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(right), colorFrame, FALSE, FALSE, 0);

    GtkWidget* fontFrame = gtk_frame_new(text(2213, "Font Style").c_str());
    GtkWidget* fg = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(fg), 6);
    gtk_grid_set_column_spacing(GTK_GRID(fg), 8);
    gtk_container_set_border_width(GTK_CONTAINER(fg), 8);
    gtk_container_add(GTK_CONTAINER(fontFrame), fg);
    GtkWidget* fontName = gtk_combo_box_text_new_with_entry();
    {
        PangoFontFamily** fams = nullptr;
        int n = 0;
        pango_context_list_families(gtk_widget_get_pango_context(dlg), &fams, &n);
        std::vector<std::string> names;
        for (int i = 0; i < n; ++i)
            names.emplace_back(pango_font_family_get_name(fams[i]));
        g_free(fams);
        std::sort(names.begin(), names.end());
        for (const std::string& f : names)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fontName), f.c_str());
    }
    GtkWidget* fontSize = gtk_combo_box_text_new_with_entry();
    for (const char* sz : {"", "5", "6", "7", "8", "9", "10", "11", "12", "14", "16", "18", "20", "22", "24", "26", "28"})
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(fontSize), sz);
    GtkWidget* bold = gtk_check_button_new_with_label(text(2204, "Bold").c_str());
    GtkWidget* italic = gtk_check_button_new_with_label(text(2205, "Italic").c_str());
    GtkWidget* underline = gtk_check_button_new_with_label(text(2218, "Underline").c_str());
    gtk_grid_attach(GTK_GRID(fg), gtk_label_new(text(2208, "Font name:").c_str()), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(fg), fontName, 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(fg), gtk_label_new(text(2209, "Font size:").c_str()), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(fg), fontSize, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(fg), bold, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(fg), italic, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(fg), underline, 2, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(right), fontFrame, FALSE, FALSE, 0);

    GtkWidget* extRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(extRow), gtk_label_new(text(2216, "User ext.:").c_str()), FALSE, FALSE, 0);
    GtkWidget* ext = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(extRow), ext, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(right), extRow, FALSE, FALSE, 0);
    GtkWidget* kwLabel = gtk_label_new(text(2221, "User-defined keywords").c_str());
    gtk_label_set_xalign(GTK_LABEL(kwLabel), 0);
    gtk_box_pack_start(GTK_BOX(right), kwLabel, FALSE, FALSE, 0);
    GtkWidget* kw = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(kw), GTK_WRAP_WORD);
    GtkWidget* kwSw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(kwSw), kw);
    gtk_box_pack_start(GTK_BOX(right), kwSw, TRUE, TRUE, 0);

    // ---- state ------------------------------------------------------------------
    struct State {
        MainWindow* w;
        std::string lexer; // "" = global styles
        int index = -1;
        bool loading = false;
        GtkListStore* langStore;
        GtkListStore* styleStore;
        GtkWidget *langView, *styleView, *fgBtn, *bgBtn, *fontName, *fontSize, *bold, *italic, *underline, *ext, *kw, *kwLabel, *extRow;
        core::Style* style()
        {
            std::vector<core::Style>* v = lexer.empty() ? &w->styles_.mutableGlobals() : w->styles_.mutableLexer(lexer);
            return v && index >= 0 && index < static_cast<int>(v->size()) ? &(*v)[static_cast<std::size_t>(index)] : nullptr;
        }
        const core::Style* defaultStyle() const { return w->styles_.globalById(STYLE_DEFAULT); }
    };
    State st{this, "", -1, false, langStore, styleStore, langView, styleView, fgBtn, bgBtn, fontName, fontSize, bold, italic, underline, ext, kw, kwLabel, extRow};

    static auto fromRgba = [](const GdkRGBA& c) {
        auto ch = [](double v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
        return ch(c.red) | (ch(c.green) << 8) | (ch(c.blue) << 16);
    };

    static auto fillLanguages = [](State& s) {
        s.loading = true;
        gtk_list_store_clear(s.langStore);
        gtk_list_store_insert_with_values(s.langStore, nullptr, -1, 0, "Global Styles", 1, "", -1);
        for (const core::StyleCatalog::LexerInfo& l : s.w->styles_.lexers())
            gtk_list_store_insert_with_values(s.langStore, nullptr, -1, 0, (l.desc.empty() ? l.name : l.desc).c_str(), 1, l.name.c_str(), -1);
        s.loading = false;
    };
    static auto showStyle = [](State& s) {
        s.loading = true;
        const core::Style* style = s.style();
        const core::Style* def = s.defaultStyle();
        const bool have = style != nullptr;
        for (GtkWidget* wdg : {s.fgBtn, s.bgBtn, s.fontName, s.fontSize, s.bold, s.italic, s.underline, s.kw})
            gtk_widget_set_sensitive(wdg, have);
        if (have) {
            const std::uint32_t fgc = style->fg ? *style->fg : (def && def->fg ? *def->fg : 0x000000);
            const std::uint32_t bgc = style->bg ? *style->bg : (def && def->bg ? *def->bg : 0xFFFFFF);
            GdkRGBA a{(fgc & 0xFF) / 255.0, ((fgc >> 8) & 0xFF) / 255.0, ((fgc >> 16) & 0xFF) / 255.0, 1};
            GdkRGBA b{(bgc & 0xFF) / 255.0, ((bgc >> 8) & 0xFF) / 255.0, ((bgc >> 16) & 0xFF) / 255.0, 1};
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s.fgBtn), &a);
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(s.bgBtn), &b);
            gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s.fontName))), style->fontName.c_str());
            gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s.fontSize))), style->fontSize ? std::to_string(*style->fontSize).c_str() : "");
            const int fs = style->fontStyle.value_or(0);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s.bold), (fs & core::kBold) != 0);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s.italic), (fs & core::kItalic) != 0);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s.underline), (fs & core::kUnderline) != 0);
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(s.kw)), style->userKeywords.c_str(), -1);
        }
        const bool keywords = have && style->keywordClass >= 0;
        gtk_widget_set_visible(s.kw, keywords);
        gtk_widget_set_visible(gtk_widget_get_parent(s.kw), keywords);
        gtk_widget_set_visible(s.kwLabel, keywords);
        gtk_widget_set_visible(s.extRow, !s.lexer.empty());
        for (const core::StyleCatalog::LexerInfo& l : s.w->styles_.lexers())
            if (l.name == s.lexer)
                gtk_entry_set_text(GTK_ENTRY(s.ext), l.ext.c_str());
        s.loading = false;
    };
    static auto fillStyles = [](State& s) {
        s.loading = true;
        gtk_list_store_clear(s.styleStore);
        const std::vector<core::Style>* v = s.lexer.empty() ? &s.w->styles_.globals() : s.w->styles_.forLanguage(s.lexer);
        if (v)
            for (const core::Style& style : *v)
                gtk_list_store_insert_with_values(s.styleStore, nullptr, -1, 0, style.name.c_str(), 1, "", -1);
        s.index = -1;
        s.loading = false;
        GtkTreePath* p = gtk_tree_path_new_first();
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(s.styleView)), p);
        gtk_tree_path_free(p);
        showStyle(s);
    };
    // Every edit goes straight into styles_ and is shown at once.
    static auto commit = [](State& s) {
        if (s.loading)
            return;
        core::Style* style = s.style();
        if (!style)
            return;
        GdkRGBA c;
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(s.fgBtn), &c);
        const std::uint32_t fgc = fromRgba(c);
        gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(s.bgBtn), &c);
        const std::uint32_t bgc = fromRgba(c);
        if (!style->fg || *style->fg != fgc)
            style->fg = fgc;
        if (!style->bg || *style->bg != bgc)
            style->bg = bgc;
        style->fontName = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s.fontName))));
        const std::string size = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(s.fontSize))));
        style->fontSize = size.empty() ? std::nullopt : std::optional<int>(std::atoi(size.c_str()));
        style->fontStyle = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.bold)) ? core::kBold : 0) |
                           (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.italic)) ? core::kItalic : 0) |
                           (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s.underline)) ? core::kUnderline : 0);
        GtkTextBuffer* tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s.kw));
        GtkTextIter a, b;
        gtk_text_buffer_get_bounds(tb, &a, &b);
        gchar* kwText = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
        style->userKeywords = kwText ? kwText : "";
        g_free(kwText);
        if (!s.lexer.empty())
            s.w->styles_.setLexerExt(s.lexer, gtk_entry_get_text(GTK_ENTRY(s.ext)));
        s.w->restyleAll();
    };

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(langView)), "changed", G_CALLBACK(+[](GtkTreeSelection* sel, gpointer p) {
                         auto& s = *static_cast<State*>(p);
                         GtkTreeModel* m = nullptr;
                         GtkTreeIter it;
                         if (s.loading || !gtk_tree_selection_get_selected(sel, &m, &it))
                             return;
                         gchar* key = nullptr;
                         gtk_tree_model_get(m, &it, 1, &key, -1);
                         s.lexer = key ? key : "";
                         g_free(key);
                         fillStyles(s);
                     }), &st);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(styleView)), "changed", G_CALLBACK(+[](GtkTreeSelection* sel, gpointer p) {
                         auto& s = *static_cast<State*>(p);
                         GtkTreeModel* m = nullptr;
                         GtkTreeIter it;
                         if (s.loading || !gtk_tree_selection_get_selected(sel, &m, &it))
                             return;
                         GtkTreePath* path = gtk_tree_model_get_path(m, &it);
                         s.index = gtk_tree_path_get_indices(path)[0];
                         gtk_tree_path_free(path);
                         showStyle(s);
                     }), &st);
    auto changed = G_CALLBACK(+[](GtkWidget*, gpointer p) { commit(*static_cast<State*>(p)); });
    g_signal_connect(fgBtn, "color-set", changed, &st);
    g_signal_connect(bgBtn, "color-set", changed, &st);
    g_signal_connect(fontName, "changed", changed, &st);
    g_signal_connect(fontSize, "changed", changed, &st);
    for (GtkWidget* t : {bold, italic, underline})
        g_signal_connect(t, "toggled", changed, &st);
    g_signal_connect(ext, "changed", changed, &st);
    g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(kw)), "changed", G_CALLBACK(+[](gpointer p) {
                                 commit(*static_cast<State*>(p));
                             }), &st);
    struct ThemeCtx {
        State* s;
        std::string* theme;
    } tctx{&st, &theme};
    g_signal_connect(themes, "changed", G_CALLBACK(+[](GtkComboBox* c, gpointer p) {
                         auto& t = *static_cast<ThemeCtx*>(p);
                         const char* id = gtk_combo_box_get_active_id(c);
                         *t.theme = id ? id : "";
                         std::string err;
                         if (!t.s->w->styles_.load(t.s->w->themePath(*t.theme), &err))
                             g_warning("%s", err.c_str());
                         t.s->lexer.clear();
                         fillLanguages(*t.s);
                         GtkTreePath* path = gtk_tree_path_new_first();
                         gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(t.s->langView)), path);
                         gtk_tree_path_free(path);
                         fillStyles(*t.s);
                         t.s->w->restyleAll();
                     }), &tctx);

    fillLanguages(st);
    gtk_widget_show_all(content);
    GtkTreePath* first = gtk_tree_path_new_first();
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(langView)), first);
    gtk_tree_path_free(first);
    fillStyles(st);

    const gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (r == GTK_RESPONSE_OK) {
        // Saved where upstream keeps user styles: <config>/stylers.xml, or the
        // user's copy of the theme.
        const fs::path target = theme.empty() ? platform::userConfigDir() / "stylers.xml" : platform::userConfigDir() / "themes" / theme;
        std::string err;
        if (!styles_.save(target, &err))
            showError("Cannot save the styles", err);
        settings_.theme = theme;
        settings_.save();
    }
    loadStyles(); // Cancel: back to what is on disk
    restyleAll();
    syncActionStates();
}

} // namespace npp::ui

// ============================================================================
// Column Editor (upstream ColumnEditorDlg)
// ============================================================================

namespace npp::ui {

void MainWindow::cmdColumnEditor()
{
    EditorView* v = activeView();
    if (!v || v->execute(SCI_GETREADONLY))
        return;
    const char* sec = "ColumnEditor";
    auto t = [&](int id, const char* fb) { return core::Translator::toMnemonic(tr_.dialog(sec, id, fb)); };
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr(sec, "title", "Column Editor").c_str(), GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                                                 t(2, "Cancel").c_str(), GTK_RESPONSE_CANCEL, t(1, "OK").c_str(), GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);

    GtkWidget* textRadio = gtk_radio_button_new_with_mnemonic(nullptr, t(2023, "&Text to Insert").c_str());
    GtkWidget* text = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(text), TRUE);
    gtk_widget_set_margin_start(text, 24);
    GtkWidget* numRadio = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(textRadio), t(2033, "&Number to Insert").c_str());
    gtk_box_pack_start(GTK_BOX(box), textRadio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), numRadio, FALSE, FALSE, 0);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 24);
    auto field = [&](int row, int id, const char* fb, const char* value) {
        GtkWidget* l = gtk_label_new_with_mnemonic(t(id, fb).c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
        GtkWidget* e = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(e), value);
        gtk_entry_set_width_chars(GTK_ENTRY(e), 10);
        gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), e);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), e, 1, row, 1, 1);
        return e;
    };
    GtkWidget* initial = field(0, 2030, "&Initial number:", "1");
    GtkWidget* incr = field(1, 2031, "Increase b&y:", "1");
    GtkWidget* repeat = field(2, 2036, "&Repeat:", "1");
    GtkWidget* leadLabel = gtk_label_new_with_mnemonic(t(2038, "&Leading:").c_str());
    gtk_label_set_xalign(GTK_LABEL(leadLabel), 1.0f);
    GtkWidget* lead = gtk_combo_box_text_new();
    for (const char* s : {"None", "Zeros", "Spaces"})
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lead), s);
    gtk_combo_box_set_active(GTK_COMBO_BOX(lead), 0);
    gtk_grid_attach(GTK_GRID(grid), leadLabel, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lead, 1, 3, 1, 1);
    GtkWidget* fmt = gtk_frame_new(core::Translator::plain(tr_.dialog(sec, 2032, "Format")).c_str());
    GtkWidget* fmtBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(fmtBox), 6);
    gtk_container_add(GTK_CONTAINER(fmt), fmtBox);
    GtkWidget* dec = gtk_radio_button_new_with_mnemonic(nullptr, t(2024, "&Dec").c_str());
    GtkWidget* hex = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(dec), t(2026, "&Hex").c_str());
    GtkWidget* oct = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(dec), t(2025, "&Oct").c_str());
    GtkWidget* bin = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(dec), t(2027, "&Bin").c_str());
    GtkWidget* upper = gtk_check_button_new_with_label("A-F");
    gtk_widget_set_tooltip_text(upper, "Uppercase hexadecimal digits");
    for (GtkWidget* w : {dec, hex, oct, bin, upper})
        gtk_box_pack_start(GTK_BOX(fmtBox), w, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), fmt, 2, 0, 1, 4);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    auto sync = +[](GtkToggleButton* b, gpointer g) { gtk_widget_set_sensitive(GTK_WIDGET(g), gtk_toggle_button_get_active(b)); };
    g_signal_connect(numRadio, "toggled", G_CALLBACK(sync), grid);
    g_signal_connect(textRadio, "toggled", G_CALLBACK(sync), text);
    gtk_widget_set_sensitive(grid, FALSE);
    gtk_widget_show_all(box);

    while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textRadio))) {
            core::columnInsertText(v->sci(), gtk_entry_get_text(GTK_ENTRY(text)));
            break;
        }
        const core::NumberBase base = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hex))   ? core::NumberBase::Hex
                                      : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(oct)) ? core::NumberBase::Oct
                                      : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bin)) ? core::NumberBase::Bin
                                                                                             : core::NumberBase::Dec;
        const int radix = base == core::NumberBase::Hex ? 16 : base == core::NumberBase::Oct ? 8 : base == core::NumberBase::Bin ? 2 : 10;
        // Upstream reads the fields in the chosen base and refuses bad input.
        auto parse = [radix](GtkWidget* e, std::uint64_t& out) {
            const char* s = gtk_entry_get_text(GTK_ENTRY(e));
            char* end = nullptr;
            out = std::strtoull(s, &end, radix);
            return *s && end && *end == '\0';
        };
        std::uint64_t a = 0, b = 0, r = 0;
        GtkWidget* bad = !parse(initial, a) ? initial : !parse(incr, b) ? incr : (!parse(repeat, r) || r == 0) ? repeat : nullptr;
        if (bad) {
            gtk_widget_grab_focus(bad);
            gtk_widget_error_bell(dlg);
            continue;
        }
        core::columnInsertNumbers(v->sci(), a, b, r, base, static_cast<core::Leading>(gtk_combo_box_get_active(GTK_COMBO_BOX(lead))),
                                  gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(upper)));
        break;
    }
    gtk_widget_destroy(dlg);
    refreshStatus();
}

} // namespace npp::ui
