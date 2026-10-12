#include "ui/DocListPanel.h"

#include <filesystem>

namespace npp::ui {

namespace {
enum Col { kColName, kColExt, kColPath, kColView, kColIndex, kColIcon, kColCount };
}

DocListPanel::DocListPanel(const core::Translator& tr, std::function<void(int, int)> activate, std::function<void(int, int)> close)
    : activate_(std::move(activate)), close_(std::move(close))
{
    box_ = gtk_scrolled_window_new(nullptr, nullptr);
    g_object_ref_sink(box_);
    store_ = gtk_list_store_new(kColCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    auto column = [&](const char* key, const char* fallback, int col, bool expand) {
        GtkTreeViewColumn* c = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(c, tr.dialogAttr(std::string("DocList/") + key, "name", fallback).c_str());
        if (col == kColName) {
            GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(c, icon, FALSE);
            gtk_tree_view_column_add_attribute(c, icon, "icon-name", kColIcon);
        }
        GtkCellRenderer* t = gtk_cell_renderer_text_new();
        g_object_set(t, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, nullptr);
        gtk_tree_view_column_pack_start(c, t, TRUE);
        gtk_tree_view_column_add_attribute(c, t, "text", col);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_expand(c, expand);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), c);
    };
    column("ColumnName", "Name", kColName, TRUE);
    column("ColumnExt", "Ext.", kColExt, FALSE);
    // Upstream shows Name and Ext. by default; the path is the tooltip.
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(tree_), kColPath);
    gtk_container_add(GTK_CONTAINER(box_), tree_);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), "changed", G_CALLBACK(+[](GtkTreeSelection* sel, gpointer p) {
                         auto* self = static_cast<DocListPanel*>(p);
                         GtkTreeModel* m = nullptr;
                         GtkTreeIter it;
                         if (self->updating_ || !gtk_tree_selection_get_selected(sel, &m, &it))
                             return;
                         int view = 0;
                         int index = 0;
                         gtk_tree_model_get(m, &it, kColView, &view, kColIndex, &index, -1);
                         if (self->activate_)
                             self->activate_(view, index);
                     }), this);
    g_signal_connect(tree_, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<DocListPanel*>(p);
                         if (ev->button != 2)
                             return FALSE;
                         GtkTreePath* path = nullptr;
                         if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), static_cast<gint>(ev->x), static_cast<gint>(ev->y), &path, nullptr,
                                                            nullptr, nullptr))
                             return FALSE;
                         GtkTreeIter it;
                         int view = 0;
                         int index = 0;
                         if (gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store_), &it, path))
                             gtk_tree_model_get(GTK_TREE_MODEL(self->store_), &it, kColView, &view, kColIndex, &index, -1);
                         gtk_tree_path_free(path);
                         if (self->close_)
                             self->close_(view, index);
                         return TRUE;
                     }), this);
}

DocListPanel::~DocListPanel()
{
    g_object_unref(store_);
    g_object_unref(box_);
}

void DocListPanel::update(const std::vector<Entry>& entries)
{
    updating_ = true;
    gtk_list_store_clear(store_);
    GtkTreeIter active;
    bool haveActive = false;
    for (const Entry& e : entries) {
        GtkTreeIter it;
        const std::string ext = std::filesystem::path(e.name).extension().string();
        gtk_list_store_insert_with_values(store_, &it, -1, kColName, e.name.c_str(), kColExt, ext.c_str(), kColPath, e.path.c_str(), kColView,
                                          e.view, kColIndex, e.index, kColIcon, e.modified ? "document-save" : "text-x-generic", -1);
        if (e.active) {
            active = it;
            haveActive = true;
        }
    }
    if (haveActive)
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &active);
    updating_ = false;
}

std::string DocListPanel::dump() const
{
    std::string out;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &it))
        do {
            gchar *name = nullptr, *icon = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColName, &name, kColIcon, &icon, -1);
            const bool sel = gtk_tree_selection_iter_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &it);
            out += std::string(sel ? ">" : " ") + (std::string(icon ? icon : "") == "document-save" ? "*" : "") + (name ? name : "") + "\n";
            g_free(name);
            g_free(icon);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
    return out;
}

} // namespace npp::ui
