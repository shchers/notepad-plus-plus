#include "ui/FolderPanel.h"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

enum Col { kColName, kColPath, kColIsDir, kColLoaded, kColIcon, kColCount };

void setClipboard(const std::string& s)
{
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.c_str(), static_cast<gint>(s.size()));
}

std::string pathOf(GtkTreeModel* m, GtkTreeIter* it)
{
    gchar* p = nullptr;
    gtk_tree_model_get(m, it, kColPath, &p, -1);
    std::string s = p ? p : "";
    g_free(p);
    return s;
}

} // namespace

FolderPanel::FolderPanel(const core::Translator& tr, FolderPanelHost host) : tr_(tr), host_(std::move(host))
{
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_object_ref_sink(box_);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    auto button = [&](const char* icon, const char* tipKey, const char* tip, void (*fn)(FolderPanel*)) {
        GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(b, tr_.dialogAttr(std::string("FolderAsWorkspace/") + tipKey, "name", tip).c_str());
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(fn), this);
        gtk_box_pack_end(GTK_BOX(bar), b, FALSE, FALSE, 0);
    };
    button("view-restore-symbolic", "CollapseAllFoldersTip", "Fold all", +[](FolderPanel* p) { p->collapseAll(); });
    button("view-fullscreen-symbolic", "ExpandAllFoldersTip", "Unfold all", +[](FolderPanel* p) { p->expandAll(); });
    button("find-location-symbolic", "LocateCurrentFileTip", "Locate current file", +[](FolderPanel* p) {
        if (p->host_.currentFile)
            p->locate(p->host_.currentFile());
    });
    gtk_box_pack_start(GTK_BOX(box_), bar, FALSE, FALSE, 0);

    store_ = gtk_tree_store_new(kColCount, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_), FALSE);
    GtkTreeViewColumn* col = gtk_tree_view_column_new();
    GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* txt = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, txt, TRUE);
    gtk_tree_view_column_add_attribute(col, icon, "icon-name", kColIcon);
    gtk_tree_view_column_add_attribute(col, txt, "text", kColName);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), col);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(tree_), kColName);
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(sw), tree_);
    gtk_box_pack_start(GTK_BOX(box_), sw, TRUE, TRUE, 0);

    g_signal_connect(tree_, "test-expand-row", G_CALLBACK(+[](GtkTreeView*, GtkTreeIter* it, GtkTreePath*, gpointer p) -> gboolean {
                         static_cast<FolderPanel*>(p)->ensureLoaded(it);
                         return FALSE;
                     }), this);
    g_signal_connect(tree_, "row-activated", G_CALLBACK(+[](GtkTreeView* tv, GtkTreePath* path, GtkTreeViewColumn*, gpointer p) {
                         auto* self = static_cast<FolderPanel*>(p);
                         GtkTreeIter it;
                         if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store_), &it, path))
                             return;
                         gboolean dir = FALSE;
                         gtk_tree_model_get(GTK_TREE_MODEL(self->store_), &it, kColIsDir, &dir, -1);
                         if (dir) {
                             if (gtk_tree_view_row_expanded(tv, path))
                                 gtk_tree_view_collapse_row(tv, path);
                             else
                                 gtk_tree_view_expand_row(tv, path, FALSE);
                         } else if (self->host_.openFile) {
                             self->host_.openFile(pathOf(GTK_TREE_MODEL(self->store_), &it));
                         }
                     }), this);
    g_signal_connect(tree_, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer p) -> gboolean {
                         if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
                             return FALSE;
                         GtkTreePath* path = nullptr;
                         if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), static_cast<gint>(ev->x), static_cast<gint>(ev->y), &path, nullptr,
                                                           nullptr, nullptr)) {
                             gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
                             gtk_tree_path_free(path);
                         } else {
                             gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)));
                         }
                         static_cast<FolderPanel*>(p)->popupMenu(ev);
                         return TRUE;
                     }), this);
    // Folders dropped on the panel become roots, as upstream.
    static const GtkTargetEntry uri{const_cast<gchar*>("text/uri-list"), 0, 0};
    gtk_drag_dest_set(tree_, GTK_DEST_DEFAULT_ALL, &uri, 1, GDK_ACTION_COPY);
    g_signal_connect(tree_, "drag-data-received", G_CALLBACK(+[](GtkWidget*, GdkDragContext* ctx, gint, gint, GtkSelectionData* data, guint, guint t, gpointer p) {
                         auto* self = static_cast<FolderPanel*>(p);
                         gchar** uris = gtk_selection_data_get_uris(data);
                         for (gchar** u = uris; u && *u; ++u)
                             if (gchar* fn = g_filename_from_uri(*u, nullptr, nullptr)) {
                                 std::error_code ec;
                                 if (fs::is_directory(fn, ec))
                                     self->addRoot(fn);
                                 g_free(fn);
                             }
                         g_strfreev(uris);
                         gtk_drag_finish(ctx, TRUE, FALSE, t);
                     }), this);
}

FolderPanel::~FolderPanel()
{
    for (auto& [dir, m] : monitors_) {
        g_signal_handlers_disconnect_by_data(m, this);
        g_object_unref(m);
    }
    g_object_unref(store_);
    g_object_unref(box_);
}

std::string FolderPanel::text(int id, const char* fallback) const
{
    return core::Translator::plain(tr_.dialog("FolderAsWorkspace/Menu", id, fallback));
}

void FolderPanel::fill(GtkTreeIter* parent, const fs::path& dir)
{
    // Remove what is there, then list: folders first, then files, by name.
    GtkTreeIter child;
    while (gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &child, parent))
        gtk_tree_store_remove(store_, &child);
    std::vector<fs::directory_entry> dirs, files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') // hidden on Linux
            continue;
        std::error_code ec2;
        (e.is_directory(ec2) ? dirs : files).push_back(e);
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return g_ascii_strcasecmp(a.path().filename().c_str(), b.path().filename().c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    GtkTreeIter it, dummy;
    for (const auto& d : dirs) {
        gtk_tree_store_insert_with_values(store_, &it, parent, -1, kColName, d.path().filename().c_str(), kColPath, d.path().c_str(), kColIsDir,
                                          TRUE, kColLoaded, FALSE, kColIcon, "folder", -1);
        gtk_tree_store_insert_with_values(store_, &dummy, &it, -1, kColName, "", kColPath, "", kColIsDir, FALSE, -1); // expander
    }
    for (const auto& f : files)
        gtk_tree_store_insert_with_values(store_, &it, parent, -1, kColName, f.path().filename().c_str(), kColPath, f.path().c_str(), kColIsDir,
                                          FALSE, kColLoaded, TRUE, kColIcon, "text-x-generic", -1);
    watch(dir);
}

void FolderPanel::ensureLoaded(GtkTreeIter* it)
{
    gboolean loaded = FALSE, isDir = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store_), it, kColLoaded, &loaded, kColIsDir, &isDir, -1);
    if (loaded || !isDir)
        return;
    fill(it, pathOf(GTK_TREE_MODEL(store_), it));
    gtk_tree_store_set(store_, it, kColLoaded, TRUE, -1);
}

void FolderPanel::watch(const fs::path& dir)
{
    if (monitors_.count(dir.string()))
        return;
    GFile* f = g_file_new_for_path(dir.c_str());
    GFileMonitor* m = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (!m)
        return;
    g_object_set_data_full(G_OBJECT(m), "npp-dir", g_strdup(dir.c_str()), g_free);
    g_signal_connect(m, "changed", G_CALLBACK(+[](GFileMonitor* mon, GFile*, GFile*, GFileMonitorEvent ev, gpointer p) {
                         if (ev == G_FILE_MONITOR_EVENT_CHANGED || ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
                             ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
                             return; // contents, not the listing
                         static_cast<FolderPanel*>(p)->refresh(static_cast<const char*>(g_object_get_data(G_OBJECT(mon), "npp-dir")));
                     }), this);
    monitors_[dir.string()] = m;
}

bool FolderPanel::findRow(const fs::path& p, GtkTreeIter* out) const
{
    const std::string target = p.lexically_normal().string();
    std::function<bool(GtkTreeIter*)> walk = [&](GtkTreeIter* parent) {
        GtkTreeIter it;
        if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &it, parent))
            return false;
        do {
            const std::string path = pathOf(GTK_TREE_MODEL(store_), &it);
            if (path == target) {
                *out = it;
                return true;
            }
            if (!path.empty() && target.compare(0, path.size(), path) == 0 && target.size() > path.size() && target[path.size()] == '/' &&
                walk(&it))
                return true;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
        return false;
    };
    return walk(nullptr);
}

void FolderPanel::refresh(const fs::path& dir)
{
    GtkTreeIter it;
    if (!findRow(dir, &it))
        return;
    gboolean loaded = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColLoaded, &loaded, -1);
    if (!loaded)
        return;
    // Keep the expanded sub-folders expanded.
    core::Session s;
    saveTo(s);
    fill(&it, dir);
    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), path, FALSE);
    gtk_tree_path_free(path);
    for (const core::FileBrowserRoot& r : s.fileBrowserRoots)
        for (const std::string& e : r.expanded) {
            GtkTreeIter sub;
            if (e.size() > dir.string().size() && findRow(e, &sub)) {
                GtkTreePath* sp = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &sub);
                gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), sp, FALSE);
                gtk_tree_path_free(sp);
            }
        }
}

void FolderPanel::addRoot(const fs::path& d)
{
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(d, ec);
    if (!fs::is_directory(dir, ec))
        return;
    GtkTreeIter it;
    if (findRow(dir, &it) && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store_), nullptr) > 0) {
        GtkTreeIter top;
        bool isRoot = false;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &top))
            do
                isRoot = isRoot || pathOf(GTK_TREE_MODEL(store_), &top) == dir.string();
            while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &top));
        if (isRoot)
            return; // already there
    }
    gtk_tree_store_insert_with_values(store_, &it, nullptr, -1, kColName, dir.filename().empty() ? dir.c_str() : dir.filename().c_str(),
                                      kColPath, dir.c_str(), kColIsDir, TRUE, kColLoaded, TRUE, kColIcon, "folder", -1);
    fill(&it, dir);
    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), path, FALSE);
    gtk_tree_path_free(path);
}

void FolderPanel::removeRoot(const fs::path& dir)
{
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &it))
        return;
    do {
        if (pathOf(GTK_TREE_MODEL(store_), &it) == dir.string()) {
            gtk_tree_store_remove(store_, &it);
            break;
        }
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
    // Monitors of folders no longer shown go.
    for (auto i = monitors_.begin(); i != monitors_.end();) {
        GtkTreeIter row;
        if (!findRow(i->first, &row)) {
            g_signal_handlers_disconnect_by_data(i->second, this);
            g_object_unref(i->second);
            i = monitors_.erase(i);
        } else {
            ++i;
        }
    }
}

void FolderPanel::clear()
{
    for (const fs::path& r : roots())
        removeRoot(r);
}

std::vector<fs::path> FolderPanel::roots() const
{
    std::vector<fs::path> out;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &it))
        do
            out.emplace_back(pathOf(GTK_TREE_MODEL(store_), &it));
        while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
    return out;
}

bool FolderPanel::locate(const fs::path& file)
{
    if (file.empty())
        return false;
    const fs::path target = file.lexically_normal();
    for (const fs::path& r : roots()) {
        const std::string rs = r.string(), ts = target.string();
        if (ts.compare(0, rs.size(), rs) != 0 || ts.size() <= rs.size() || ts[rs.size()] != '/')
            continue;
        // Expand each folder on the way down.
        fs::path cur = r;
        for (const fs::path& part : target.lexically_relative(r)) {
            GtkTreeIter it;
            if (!findRow(cur, &it))
                return false;
            ensureLoaded(&it);
            GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), path, FALSE);
            gtk_tree_path_free(path);
            cur /= part;
        }
        GtkTreeIter it;
        if (!findRow(target, &it))
            return false;
        GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree_), path, nullptr, TRUE, 0.5f, 0);
        gtk_tree_path_free(path);
        return true;
    }
    return false;
}

void FolderPanel::expandAll()
{
    // Upstream unfolds what is loaded; loading everything could be huge.
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_));
}

void FolderPanel::collapseAll()
{
    gtk_tree_view_collapse_all(GTK_TREE_VIEW(tree_));
}

fs::path FolderPanel::selectedPath() const
{
    GtkTreeModel* m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &m, &it))
        return {};
    return pathOf(m, &it);
}

void FolderPanel::saveTo(core::Session& s) const
{
    s.fileBrowserRoots.clear();
    s.fileBrowserSelected = selectedPath().string();
    GtkTreeIter root;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &root))
        return;
    do {
        core::FileBrowserRoot r{pathOf(GTK_TREE_MODEL(store_), &root), {}};
        std::function<void(GtkTreeIter*)> walk = [&](GtkTreeIter* parent) {
            GtkTreeIter it;
            if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &it, parent))
                return;
            do {
                gboolean dir = FALSE;
                gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColIsDir, &dir, -1);
                if (!dir)
                    continue;
                GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
                if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree_), path)) {
                    r.expanded.push_back(pathOf(GTK_TREE_MODEL(store_), &it));
                    walk(&it);
                }
                gtk_tree_path_free(path);
            } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
        };
        walk(&root);
        s.fileBrowserRoots.push_back(std::move(r));
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &root));
}

void FolderPanel::restoreFrom(const core::Session& s)
{
    clear();
    for (const core::FileBrowserRoot& r : s.fileBrowserRoots) {
        addRoot(r.folder);
        for (const std::string& e : r.expanded) {
            GtkTreeIter it;
            if (!findRow(e, &it))
                continue;
            ensureLoaded(&it);
            GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), path, FALSE);
            gtk_tree_path_free(path);
        }
    }
    if (!s.fileBrowserSelected.empty())
        locate(s.fileBrowserSelected);
}

std::string FolderPanel::dump() const
{
    std::string out;
    std::function<void(GtkTreeIter*, int)> walk = [&](GtkTreeIter* parent, int depth) {
        GtkTreeIter it;
        if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &it, parent))
            return;
        do {
            gchar* name = nullptr;
            gboolean dir = FALSE;
            gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColName, &name, kColIsDir, &dir, -1);
            if (name && *name) {
                out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + name + (dir ? "/" : "") + "\n";
                GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &it);
                if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(tree_), path))
                    walk(&it, depth + 1);
                gtk_tree_path_free(path);
            }
            g_free(name);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
    };
    walk(nullptr, 0);
    return out;
}

void FolderPanel::popupMenu(GdkEventButton* ev)
{
    GtkTreeModel* m = nullptr;
    GtkTreeIter it;
    const bool sel = gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &m, &it);
    std::string path;
    gboolean isDir = FALSE;
    bool isRoot = false;
    if (sel) {
        path = pathOf(m, &it);
        gtk_tree_model_get(m, &it, kColIsDir, &isDir, -1);
        GtkTreeIter parent;
        isRoot = !gtk_tree_model_iter_parent(m, &parent, &it);
    }
    GtkWidget* menu = gtk_menu_new();
    struct Item {
        FolderPanel* self;
        std::string path;
        int id;
    };
    auto add = [&](int id, const char* fallback) {
        GtkWidget* mi = gtk_menu_item_new_with_label(text(id, fallback).c_str());
        auto* data = new Item{this, path, id};
        g_signal_connect_data(mi, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer p) {
                                  auto* d = static_cast<Item*>(p);
                                  FolderPanel* s = d->self;
                                  const fs::path pth = d->path;
                                  switch (d->id) {
                                  case 3511: s->removeRoot(pth); break;
                                  case 3512: s->clear(); break;
                                  case 3513:
                                      if (s->host_.chooseFolder)
                                          if (const fs::path f = s->host_.chooseFolder(); !f.empty())
                                              s->addRoot(f);
                                      break;
                                  case 3514: // Run by system: the desktop's handler
                                      if (s->host_.openUri)
                                          if (gchar* uri = g_filename_to_uri(pth.c_str(), nullptr, nullptr)) {
                                              s->host_.openUri(uri);
                                              g_free(uri);
                                          }
                                      break;
                                  case 3515:
                                      if (s->host_.openFile)
                                          s->host_.openFile(pth);
                                      break;
                                  case 3516: setClipboard(pth.string()); break;
                                  case 3520: setClipboard(pth.filename().string()); break;
                                  case 3517:
                                      if (s->host_.findInFiles)
                                          s->host_.findInFiles(pth);
                                      break;
                                  case 3518: // file manager here
                                      if (s->host_.openUri)
                                          if (gchar* uri = g_filename_to_uri(pth.c_str(), nullptr, nullptr)) {
                                              s->host_.openUri(uri);
                                              g_free(uri);
                                          }
                                      break;
                                  case 3519:
                                      if (s->host_.openTerminal)
                                          s->host_.openTerminal(pth);
                                      break;
                                  default: break;
                                  }
                              }), data, +[](gpointer p, GClosure*) { delete static_cast<Item*>(p); }, GConnectFlags(0));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    };
    auto sep = [&] { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };
    // Upstream's menus: for a root, a folder, a file, or the empty area.
    if (!sel) {
        add(3513, "Add");
        add(3512, "Remove All");
    } else if (isDir) {
        if (isRoot) {
            add(3511, "Remove");
            sep();
        }
        add(3516, "Copy path");
        add(3520, "Copy file name");
        add(3517, "Find in Files...");
        sep();
        add(3518, "File manager here");
        add(3519, "Terminal here");
    } else {
        add(3515, "Open");
        add(3516, "Copy path");
        add(3520, "Copy file name");
        add(3514, "Run by system");
        sep();
        path = fs::path(path).parent_path().string();
        add(3518, "File manager here");
        add(3519, "Terminal here");
    }
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), tree_, nullptr);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* sh, gpointer) {
                         g_idle_add(+[](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, sh);
                     }), nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), reinterpret_cast<GdkEvent*>(ev));
}

} // namespace npp::ui
