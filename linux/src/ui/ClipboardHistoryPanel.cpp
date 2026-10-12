#include "ui/ClipboardHistoryPanel.h"

#include <algorithm>

namespace npp::ui {

ClipboardHistoryPanel::ClipboardHistoryPanel(std::function<void(const std::string&)> paste) : paste_(std::move(paste))
{
    box_ = gtk_scrolled_window_new(nullptr, nullptr);
    g_object_ref_sink(box_);
    store_ = gtk_list_store_new(1, G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_), FALSE);
    GtkCellRenderer* r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), gtk_tree_view_column_new_with_attributes("", r, "text", 0, nullptr));
    gtk_container_add(GTK_CONTAINER(box_), tree_);
    g_signal_connect(tree_, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer p) {
                         auto* self = static_cast<ClipboardHistoryPanel*>(p);
                         const auto i = static_cast<std::size_t>(gtk_tree_path_get_indices(path)[0]);
                         if (i < self->entries_.size() && self->paste_)
                             self->paste_(self->entries_[i]);
                     }), this);
    // Every new clipboard owner: read its text.
    clipboard_ = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    alive_ = std::make_shared<ClipboardHistoryPanel*>(this);
    handler_ = g_signal_connect(clipboard_, "owner-change", G_CALLBACK(+[](GtkClipboard* cb, GdkEvent*, gpointer p) {
                                    auto* self = static_cast<ClipboardHistoryPanel*>(p);
                                    auto* weak = new std::weak_ptr<ClipboardHistoryPanel*>(self->alive_);
                                    gtk_clipboard_request_text(cb, +[](GtkClipboard*, const gchar* text, gpointer w) {
                                        auto* weakPtr = static_cast<std::weak_ptr<ClipboardHistoryPanel*>*>(w);
                                        if (auto alive = weakPtr->lock(); alive && text && *text)
                                            (*alive)->add(text);
                                        delete weakPtr;
                                    }, weak);
                                }), this);
}

ClipboardHistoryPanel::~ClipboardHistoryPanel()
{
    g_signal_handler_disconnect(clipboard_, handler_);
    g_object_unref(store_);
    g_object_unref(box_);
}

void ClipboardHistoryPanel::add(const std::string& text)
{
    entries_.erase(std::remove(entries_.begin(), entries_.end(), text), entries_.end());
    entries_.push_front(text);
    while (entries_.size() > kMax)
        entries_.pop_back();
    rebuild();
}

void ClipboardHistoryPanel::rebuild()
{
    gtk_list_store_clear(store_);
    for (const std::string& e : entries_) {
        // One line per entry, as upstream shows it.
        std::string line;
        if (g_utf8_validate(e.c_str(), static_cast<gssize>(e.size()), nullptr)) {
            gchar* head = g_utf8_substring(e.c_str(), 0, std::min<glong>(200, g_utf8_strlen(e.c_str(), static_cast<gssize>(e.size()))));
            line = head;
            g_free(head);
        }
        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        std::replace(line.begin(), line.end(), '\t', ' ');
        if (line.empty())
            line = "?";
        gtk_list_store_insert_with_values(store_, nullptr, -1, 0, line.c_str(), -1);
    }
}

} // namespace npp::ui
