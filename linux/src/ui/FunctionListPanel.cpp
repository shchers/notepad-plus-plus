#include "ui/FunctionListPanel.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace npp::ui {

namespace {

enum Col { kColText, kColPos, kColCount };

bool contains(const std::string& hay, const std::string& needle)
{
    if (needle.empty())
        return true;
    gchar* h = g_utf8_casefold(hay.c_str(), -1);
    gchar* n = g_utf8_casefold(needle.c_str(), -1);
    const bool found = std::strstr(h, n) != nullptr;
    g_free(h);
    g_free(n);
    return found;
}

} // namespace

FunctionListPanel::FunctionListPanel(core::FunctionParsers& parsers, const core::Translator& tr, std::function<Document()> current)
    : parsers_(parsers), tr_(tr), current_(std::move(current))
{
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_object_ref_sink(box_);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    search_ = gtk_search_entry_new();
    gtk_widget_set_hexpand(search_, TRUE);
    sort_ = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(sort_), gtk_image_new_from_icon_name("view-sort-ascending-symbolic", GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(sort_, tr_.misc("functionlist-sort", "Sort").c_str());
    GtkWidget* reloadBtn = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(reloadBtn, tr_.misc("functionlist-reload", "Reload").c_str());
    gtk_box_pack_start(GTK_BOX(bar), search_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bar), sort_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), reloadBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box_), bar, FALSE, FALSE, 0);

    store_ = gtk_tree_store_new(kColCount, G_TYPE_STRING, G_TYPE_INT64);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree_), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", kColText, nullptr));
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(sw), tree_);
    gtk_box_pack_start(GTK_BOX(box_), sw, TRUE, TRUE, 0);

    g_signal_connect_swapped(search_, "search-changed", G_CALLBACK(+[](FunctionListPanel* p) { p->rebuild(); }), this);
    g_signal_connect_swapped(sort_, "toggled", G_CALLBACK(+[](FunctionListPanel* p) { p->rebuild(); }), this);
    g_signal_connect_swapped(reloadBtn, "clicked", G_CALLBACK(+[](FunctionListPanel* p) { p->reload(); }), this);
    g_signal_connect(tree_, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer p) {
                         static_cast<FunctionListPanel*>(p)->openRow(path);
                     }), this);
}

FunctionListPanel::~FunctionListPanel()
{
    if (timer_)
        g_source_remove(timer_);
    g_object_unref(store_);
    g_object_unref(box_);
}

void FunctionListPanel::scheduleReload()
{
    if (timer_)
        g_source_remove(timer_);
    timer_ = g_timeout_add(1000, +[](gpointer p) -> gboolean {
        auto* self = static_cast<FunctionListPanel*>(p);
        self->timer_ = 0;
        self->reload();
        return G_SOURCE_REMOVE;
    }, this);
}

void FunctionListPanel::reload()
{
    infos_.clear();
    root_.clear();
    const Document d = current_ ? current_() : Document{};
    if (d.view && parsers_.parse(d.view->sci(), d.langName, d.udlName, infos_))
        root_ = d.name;
    rebuild();
}

void FunctionListPanel::setSorted(bool on)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sort_), on);
}

void FunctionListPanel::setFilter(const std::string& text)
{
    gtk_entry_set_text(GTK_ENTRY(search_), text.c_str());
    rebuild();
}

void FunctionListPanel::rebuild()
{
    gtk_tree_store_clear(store_);
    if (root_.empty())
        return;
    const std::string filter = gtk_entry_get_text(GTK_ENTRY(search_));
    const bool sorted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sort_));
    // Upstream addEntry: classes become nodes under the root, in order of
    // first appearance; functions are leaves.
    std::vector<std::pair<std::string, std::vector<const core::FunctionInfo*>>> nodes;
    std::vector<const core::FunctionInfo*> leaves;
    for (const core::FunctionInfo& fi : infos_) {
        if (!contains(fi.name, filter) && !(fi.className.size() && contains(fi.className, filter)))
            continue;
        if (fi.className.empty()) {
            leaves.push_back(&fi);
            continue;
        }
        auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& n) { return n.first == fi.className; });
        if (it == nodes.end())
            nodes.push_back({fi.className, {&fi}});
        else
            it->second.push_back(&fi);
    }
    auto byName = [](const core::FunctionInfo* a, const core::FunctionInfo* b) { return g_utf8_collate(a->name.c_str(), b->name.c_str()) < 0; };
    if (sorted) {
        std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) { return g_utf8_collate(a.first.c_str(), b.first.c_str()) < 0; });
        for (auto& n : nodes)
            std::stable_sort(n.second.begin(), n.second.end(), byName);
        std::stable_sort(leaves.begin(), leaves.end(), byName);
    }
    GtkTreeIter root, node, leaf;
    gtk_tree_store_insert_with_values(store_, &root, nullptr, -1, kColText, root_.c_str(), kColPos, gint64{-1}, -1);
    for (const auto& [name, funcs] : nodes) {
        gtk_tree_store_insert_with_values(store_, &node, &root, -1, kColText, name.c_str(), kColPos, gint64{-1}, -1);
        for (const core::FunctionInfo* f : funcs)
            gtk_tree_store_insert_with_values(store_, &leaf, &node, -1, kColText, f->name.c_str(), kColPos, static_cast<gint64>(f->pos), -1);
    }
    for (const core::FunctionInfo* f : leaves)
        gtk_tree_store_insert_with_values(store_, &leaf, &root, -1, kColText, f->name.c_str(), kColPos, static_cast<gint64>(f->pos), -1);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_));
}

bool FunctionListPanel::openRow(GtkTreePath* path)
{
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(store_), &it, path))
        return false;
    gint64 pos = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColPos, &pos, -1);
    const Document d = current_ ? current_() : Document{};
    if (pos < 0 || !d.view)
        return false;
    // Upstream openSelection: unfold, then centre the function in the view.
    EditorView& v = *d.view;
    const sptr_t line = v.lineFromPos(static_cast<sptr_t>(pos));
    v.execute(SCI_ENSUREVISIBLE, static_cast<uptr_t>(line));
    v.execute(SCI_GOTOPOS, static_cast<uptr_t>(pos));
    const sptr_t first = v.execute(SCI_VISIBLEFROMDOCLINE, static_cast<uptr_t>(line)) - v.execute(SCI_LINESONSCREEN) / 2;
    v.execute(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(std::max<sptr_t>(first, 0)));
    gtk_widget_grab_focus(v.widget());
    return true;
}

std::string FunctionListPanel::dump() const
{
    std::string out;
    std::function<void(GtkTreeIter*, int)> walk = [&](GtkTreeIter* parent, int depth) {
        GtkTreeIter it;
        if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &it, parent))
            return;
        do {
            gchar* t = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColText, &t, -1);
            out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + (t ? t : "") + "\n";
            g_free(t);
            walk(&it, depth + 1);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
    };
    walk(nullptr, 0);
    return out;
}

} // namespace npp::ui
