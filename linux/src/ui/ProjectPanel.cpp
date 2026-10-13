#include "ui/ProjectPanel.h"

#include "menuCmdID.h"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

enum Col { kColLabel, kColKind, kColPath, kColIcon, kColCount };

constexpr const char* kSection = "ProjectManager";

const char* iconFor(int kind, const std::string& path)
{
    switch (kind) {
    case -1: return "folder-documents";
    case 0: return "package-x-generic";
    case 1: return "folder";
    default: {
        std::error_code ec;
        return fs::exists(path, ec) ? "text-x-generic" : "dialog-warning"; // upstream marks missing files
    }
    }
}

} // namespace

ProjectPanel::ProjectPanel(int number, const core::Translator& tr, ProjectPanelHost host) : number_(number), tr_(tr), host_(std::move(host))
{
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_object_ref_sink(box_);

    // Upstream's two menu buttons: Workspace and Edit.
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (int i = 0; i < 2; ++i) {
        GtkWidget* b = gtk_button_new_with_label(
            core::Translator::plain(tr_.dialog(std::string(kSection) + "/Menus/Entries", i, i == 0 ? "Workspace" : "Edit")).c_str());
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        g_object_set_data(G_OBJECT(b), "npp-edit", GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer p) {
                             auto* self = static_cast<ProjectPanel*>(p);
                             const bool edit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "npp-edit")) != 0;
                             GtkWidget* menu = self->buildMenu(!edit);
                             gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(btn), nullptr);
                             gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(btn), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
                         }), this);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box_), bar, FALSE, FALSE, 0);

    store_ = gtk_tree_store_new(kColCount, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree_), FALSE);
    column_ = gtk_tree_view_column_new();
    GtkCellRenderer* icon = gtk_cell_renderer_pixbuf_new();
    text_ = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column_, icon, FALSE);
    gtk_tree_view_column_pack_start(column_, text_, TRUE);
    gtk_tree_view_column_add_attribute(column_, icon, "icon-name", kColIcon);
    gtk_tree_view_column_add_attribute(column_, text_, "text", kColLabel);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), column_);
    g_signal_connect(text_, "edited", G_CALLBACK(+[](GtkCellRendererText* r, gchar* path, gchar* text, gpointer p) {
                         auto* self = static_cast<ProjectPanel*>(p);
                         g_object_set(r, "editable", FALSE, nullptr);
                         if (text && *text)
                             self->rename(path, text);
                     }), this);
    g_signal_connect(text_, "editing-canceled", G_CALLBACK(+[](GtkCellRenderer* r, gpointer) { g_object_set(r, "editable", FALSE, nullptr); }),
                     nullptr);
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(sw), tree_);
    gtk_box_pack_start(GTK_BOX(box_), sw, TRUE, TRUE, 0);

    g_signal_connect(tree_, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer p) {
                         auto* self = static_cast<ProjectPanel*>(p);
                         GtkTreeIter it;
                         if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(self->store_), &it, path) || self->kindOf(&it) != Kind::File)
                             return;
                         gchar* fp = nullptr;
                         gtk_tree_model_get(GTK_TREE_MODEL(self->store_), &it, kColPath, &fp, -1);
                         if (fp && self->host_.openFile)
                             self->host_.openFile(fp);
                         g_free(fp);
                     }), this);
    g_signal_connect(tree_, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer p) -> gboolean {
                         if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
                             return FALSE;
                         GtkTreePath* path = nullptr;
                         if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), static_cast<gint>(ev->x), static_cast<gint>(ev->y), &path, nullptr,
                                                           nullptr, nullptr)) {
                             gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
                             gtk_tree_path_free(path);
                         }
                         static_cast<ProjectPanel*>(p)->popup(reinterpret_cast<GdkEvent*>(ev), true);
                         return TRUE;
                     }), this);
    newWorkspace();
}

ProjectPanel::~ProjectPanel()
{
    g_object_unref(store_);
    g_object_unref(box_);
}

std::string ProjectPanel::title() const
{
    // The View > Project Panels item's label, as upstream titles the panel.
    const std::string fallback = "Project Panel " + std::to_string(number_);
    return core::Translator::plain(tr_.command(IDM_VIEW_PROJECT_PANEL_1 + number_ - 1, fallback.c_str()));
}

std::string ProjectPanel::label(const char* menu, int id, const char* fallback) const
{
    return core::Translator::plain(tr_.dialog(std::string(kSection) + "/Menus/" + menu, id, fallback));
}

void ProjectPanel::setDirty(bool d)
{
    dirty_ = d;
    GtkTreeIter root;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &root))
        return;
    const std::string name = file_.empty() ? tr_.dialogAttr(std::string(kSection) + "/WorkspaceRootName", "name", "Workspace")
                                           : file_.filename().string();
    gtk_tree_store_set(store_, &root, kColLabel, (d ? "*" + name : name).c_str(), -1);
}

void ProjectPanel::newWorkspace()
{
    file_.clear();
    load({});
    setDirty(false);
}

void ProjectPanel::load(const core::Workspace& ws)
{
    gtk_tree_store_clear(store_);
    GtkTreeIter root;
    gtk_tree_store_insert_with_values(store_, &root, nullptr, -1, kColLabel, "", kColKind, -1, kColPath, "", kColIcon, iconFor(-1, ""), -1);
    for (const core::WorkspaceNode& p : ws.projects) {
        GtkTreeIter it;
        gtk_tree_store_insert_with_values(store_, &it, &root, -1, kColLabel, p.name.c_str(), kColKind, 0, kColPath, "", kColIcon, iconFor(0, ""), -1);
        addNodes(&it, p.children);
    }
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_));
    setDirty(dirty_);
}

void ProjectPanel::addNodes(GtkTreeIter* parent, const std::vector<core::WorkspaceNode>& nodes)
{
    for (const core::WorkspaceNode& n : nodes) {
        GtkTreeIter it;
        const int kind = n.kind == core::WorkspaceNode::Kind::Folder ? 1 : 2;
        gtk_tree_store_insert_with_values(store_, &it, parent, -1, kColLabel, n.name.c_str(), kColKind, kind, kColPath, n.path.c_str(), kColIcon,
                                          iconFor(kind, n.path.string()), -1);
        if (kind == 1)
            addNodes(&it, n.children);
    }
}

void ProjectPanel::readNodes(GtkTreeIter* parent, std::vector<core::WorkspaceNode>& out) const
{
    GtkTreeIter it;
    if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &it, parent))
        return;
    do {
        gchar *lbl = nullptr, *path = nullptr;
        int kind = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColLabel, &lbl, kColKind, &kind, kColPath, &path, -1);
        core::WorkspaceNode n;
        n.kind = kind == 0 ? core::WorkspaceNode::Kind::Project : kind == 1 ? core::WorkspaceNode::Kind::Folder : core::WorkspaceNode::Kind::File;
        n.name = lbl ? lbl : "";
        n.path = path ? path : "";
        g_free(lbl);
        g_free(path);
        if (kind != 2)
            readNodes(&it, n.children);
        out.push_back(std::move(n));
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
}

core::Workspace ProjectPanel::workspace() const
{
    core::Workspace ws;
    GtkTreeIter root;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &root))
        readNodes(&root, ws.projects);
    return ws;
}

bool ProjectPanel::openWorkspace(const fs::path& file)
{
    core::Workspace ws;
    std::string err;
    if (!core::loadWorkspace(file, ws, &err)) {
        if (host_.error)
            host_.error("Cannot open the workspace", err);
        return false;
    }
    file_ = fs::absolute(file);
    dirty_ = false;
    load(ws);
    return true;
}

bool ProjectPanel::saveWorkspace(const fs::path& target)
{
    fs::path f = target.empty() ? file_ : target;
    if (f.empty())
        f = chooseSave(label("WorkspaceMenu", 3126, "Save As...").c_str());
    if (f.empty())
        return false;
    std::string err;
    if (!core::saveWorkspace(f, workspace(), &err)) {
        if (host_.error)
            host_.error("Cannot save the workspace", err);
        return false;
    }
    file_ = fs::absolute(f);
    setDirty(false);
    return true;
}

bool ProjectPanel::confirmClose()
{
    if (!dirty_ || !host_.ask)
        return true;
    // Upstream ProjectPanel::checkIfNeedSave.
    const int r = host_.ask(title() + ": the workspace was modified. Do you want to save it?", file_.string(), true);
    if (r < 0)
        return false;
    return r == 0 || saveWorkspace();
}

bool ProjectPanel::iterFor(const std::string& path, GtkTreeIter* out) const
{
    if (path.empty())
        return gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), out);
    return gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store_), out, path.c_str());
}

ProjectPanel::Kind ProjectPanel::kindOf(GtkTreeIter* it) const
{
    int k = -1;
    gtk_tree_model_get(GTK_TREE_MODEL(store_), it, kColKind, &k, -1);
    return static_cast<Kind>(k);
}

std::string ProjectPanel::addProject(const std::string& name)
{
    GtkTreeIter root, it;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store_), &root))
        return {};
    gtk_tree_store_insert_with_values(store_, &it, &root, -1, kColLabel, name.c_str(), kColKind, 0, kColPath, "", kColIcon, iconFor(0, ""), -1);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_));
    setDirty(true);
    gchar* s = gtk_tree_model_get_string_from_iter(GTK_TREE_MODEL(store_), &it);
    std::string out = s;
    g_free(s);
    return out;
}

std::string ProjectPanel::addFolder(const std::string& parent, const std::string& name)
{
    GtkTreeIter p, it;
    if (!iterFor(parent, &p) || (kindOf(&p) != Kind::Project && kindOf(&p) != Kind::Folder))
        return {};
    gtk_tree_store_insert_with_values(store_, &it, &p, -1, kColLabel, name.c_str(), kColKind, 1, kColPath, "", kColIcon, iconFor(1, ""), -1);
    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &p);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), path, FALSE);
    gtk_tree_path_free(path);
    setDirty(true);
    gchar* s = gtk_tree_model_get_string_from_iter(GTK_TREE_MODEL(store_), &it);
    std::string out = s;
    g_free(s);
    return out;
}

void ProjectPanel::addFiles(const std::string& parent, const std::vector<fs::path>& files)
{
    GtkTreeIter p;
    if (!iterFor(parent, &p) || (kindOf(&p) != Kind::Project && kindOf(&p) != Kind::Folder) || files.empty())
        return;
    std::vector<core::WorkspaceNode> nodes;
    for (const fs::path& f : files)
        nodes.push_back({core::WorkspaceNode::Kind::File, f.filename().string(), fs::absolute(f).lexically_normal(), {}});
    addNodes(&p, nodes);
    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), &p);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(tree_), path, FALSE);
    gtk_tree_path_free(path);
    setDirty(true);
}

void ProjectPanel::addDirectory(const std::string& parent, const fs::path& dir)
{
    // Upstream recursiveAddFilesFrom: sub-folders become folders, files files.
    GtkTreeIter p;
    if (!iterFor(parent, &p))
        return;
    std::function<core::WorkspaceNode(const fs::path&)> scan = [&](const fs::path& d) {
        core::WorkspaceNode folder{core::WorkspaceNode::Kind::Folder, d.filename().string(), {}, {}};
        std::vector<fs::directory_entry> dirs, files;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
            if (e.path().filename().string().rfind('.', 0) == 0)
                continue;
            std::error_code ec2;
            (e.is_directory(ec2) ? dirs : files).push_back(e);
        }
        auto byName = [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        for (const auto& sub : dirs)
            folder.children.push_back(scan(sub.path()));
        for (const auto& f : files)
            folder.children.push_back({core::WorkspaceNode::Kind::File, f.path().filename().string(), f.path(), {}});
        return folder;
    };
    std::error_code ec;
    const fs::path abs = fs::weakly_canonical(dir, ec);
    const core::WorkspaceNode top = scan(abs);
    addNodes(&p, top.children);
    gtk_tree_view_expand_all(GTK_TREE_VIEW(tree_));
    setDirty(true);
}

void ProjectPanel::rename(const std::string& item, const std::string& name)
{
    GtkTreeIter it;
    if (!iterFor(item, &it) || kindOf(&it) == Kind::Root || name.empty())
        return;
    if (kindOf(&it) == Kind::File) {
        // Upstream renames the file on disk too.
        gchar* p = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColPath, &p, -1);
        const fs::path from = p ? p : "";
        g_free(p);
        const fs::path to = from.parent_path() / name;
        std::error_code ec;
        if (fs::exists(from, ec)) {
            fs::rename(from, to, ec);
            if (ec) {
                if (host_.error)
                    host_.error("Cannot rename \"" + from.filename().string() + "\"", ec.message());
                return;
            }
        }
        gtk_tree_store_set(store_, &it, kColPath, to.c_str(), kColIcon, iconFor(2, to.string()), -1);
    }
    gtk_tree_store_set(store_, &it, kColLabel, name.c_str(), -1);
    setDirty(true);
}

void ProjectPanel::remove(const std::string& item)
{
    GtkTreeIter it;
    if (!iterFor(item, &it) || kindOf(&it) == Kind::Root)
        return;
    gtk_tree_store_remove(store_, &it);
    setDirty(true);
}

void ProjectPanel::move(const std::string& item, bool up)
{
    GtkTreeIter it, other;
    if (!iterFor(item, &it) || kindOf(&it) == Kind::Root)
        return;
    other = it;
    if (!(up ? gtk_tree_model_iter_previous(GTK_TREE_MODEL(store_), &other) : gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &other)))
        return;
    gtk_tree_store_swap(store_, &it, &other);
    setDirty(true);
}

std::string ProjectPanel::selected() const
{
    GtkTreeModel* m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), &m, &it))
        return {};
    gchar* s = gtk_tree_model_get_string_from_iter(m, &it);
    std::string out = s;
    g_free(s);
    return out;
}

void ProjectPanel::startRename(GtkTreeIter* it)
{
    GtkTreePath* path = gtk_tree_model_get_path(GTK_TREE_MODEL(store_), it);
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(tree_), path);
    g_object_set(text_, "editable", TRUE, nullptr);
    gtk_tree_view_set_cursor_on_cell(GTK_TREE_VIEW(tree_), path, column_, text_, TRUE);
    gtk_tree_path_free(path);
}

std::vector<fs::path> ProjectPanel::chooseFiles() const
{
    GtkWidget* top = gtk_widget_get_toplevel(box_);
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(label("ProjectMenu", 3113, "Add Files...").c_str(),
                                                            GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                            "_Add", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    std::vector<fs::path> out;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList* list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList* l = list; l; l = l->next) {
            out.emplace_back(static_cast<char*>(l->data));
            g_free(l->data);
        }
        g_slist_free(list);
    }
    g_object_unref(dlg);
    return out;
}

fs::path ProjectPanel::chooseFolder() const
{
    GtkWidget* top = gtk_widget_get_toplevel(box_);
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(label("ProjectMenu", 3117, "Add Files from Directory...").c_str(),
                                                            GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
                                                            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Add", "_Cancel");
    fs::path out;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
            out = fn;
            g_free(fn);
        }
    g_object_unref(dlg);
    return out;
}

fs::path ProjectPanel::chooseSave(const char* title) const
{
    GtkWidget* top = gtk_widget_get_toplevel(box_);
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(title, GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr, GTK_FILE_CHOOSER_ACTION_SAVE,
                                                            "_Save", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (!file_.empty())
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), file_.c_str());
    fs::path out;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
            out = fn;
            g_free(fn);
        }
    g_object_unref(dlg);
    return out;
}

GtkWidget* ProjectPanel::buildMenu(bool workspaceMenu)
{
    GtkWidget* menu = gtk_menu_new();
    struct Act {
        ProjectPanel* self;
        int id;
        std::string item;
    };
    const std::string sel = selected();
    GtkTreeIter it;
    const Kind kind = (!sel.empty() && iterFor(sel, &it)) ? kindOf(&it) : Kind::Root;
    auto add = [&](const char* sub, int id, const char* fallback) {
        GtkWidget* mi = gtk_menu_item_new_with_label(label(sub, id, fallback).c_str());
        auto* a = new Act{this, id, sel};
        g_signal_connect_data(mi, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer p) {
                                  auto* a = static_cast<Act*>(p);
                                  ProjectPanel* s = a->self;
                                  GtkTreeIter it;
                                  switch (a->id) {
                                  case 3122: // New Workspace
                                      if (s->confirmClose())
                                          s->newWorkspace();
                                      break;
                                  case 3123: { // Open Workspace
                                      if (!s->confirmClose())
                                          break;
                                      GtkWidget* top = gtk_widget_get_toplevel(s->box_);
                                      GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
                                          s->label("WorkspaceMenu", 3123, "Open Workspace").c_str(), GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
                                          GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
                                      if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
                                          if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
                                              s->openWorkspace(fn);
                                              g_free(fn);
                                          }
                                      g_object_unref(dlg);
                                      break;
                                  }
                                  case 3124: // Reload
                                      if (!s->file_.empty() &&
                                          (!s->dirty_ || !s->host_.ask ||
                                           s->host_.ask("The workspace was modified. Reload it and lose the changes?", s->file_.string(), false) == 1))
                                          s->openWorkspace(s->file_);
                                      break;
                                  case 3125: s->saveWorkspace(); break;
                                  case 3126:
                                      if (const fs::path f = s->chooseSave(s->label("WorkspaceMenu", 3126, "Save As...").c_str()); !f.empty())
                                          s->saveWorkspace(f);
                                      break;
                                  case 3127: // Save a Copy As: the file stays the same
                                      if (const fs::path f = s->chooseSave(s->label("WorkspaceMenu", 3127, "Save a Copy As...").c_str()); !f.empty()) {
                                          std::string err;
                                          if (!core::saveWorkspace(f, s->workspace(), &err) && s->host_.error)
                                              s->host_.error("Cannot save the workspace", err);
                                      }
                                      break;
                                  case 3121: {
                                      const std::string p = s->addProject(
                                          s->tr_.dialogAttr(std::string(kSection) + "/NewProjectName", "name", "Project Name"));
                                      if (s->iterFor(p, &it))
                                          s->startRename(&it);
                                      break;
                                  }
                                  case 3128:
                                      if (s->host_.findInProjects)
                                          s->host_.findInProjects(core::workspaceFiles(s->workspace()));
                                      break;
                                  case 3111:
                                      if (s->iterFor(a->item, &it))
                                          s->startRename(&it);
                                      break;
                                  case 3112: {
                                      const std::string f =
                                          s->addFolder(a->item, s->tr_.dialogAttr(std::string(kSection) + "/NewFolderName", "name", "Folder Name"));
                                      if (s->iterFor(f, &it))
                                          s->startRename(&it);
                                      break;
                                  }
                                  case 3113: s->addFiles(a->item, s->chooseFiles()); break;
                                  case 3117:
                                      if (const fs::path d = s->chooseFolder(); !d.empty())
                                          s->addDirectory(a->item, d);
                                      break;
                                  case 3114: // Remove a project / folder: upstream asks first
                                      if (!s->host_.ask || s->host_.ask("All the sub-items will be removed.\nAre you sure to remove this folder from the project?",
                                                                        "", false) == 1)
                                          s->remove(a->item);
                                      break;
                                  case 3115: s->remove(a->item); break;
                                  case 3116: { // Modify File Path
                                      const auto files = s->chooseFiles();
                                      if (!files.empty() && s->iterFor(a->item, &it)) {
                                          const fs::path f = fs::absolute(files.front());
                                          gtk_tree_store_set(s->store_, &it, kColPath, f.c_str(), kColLabel, f.filename().c_str(), kColIcon,
                                                             iconFor(2, f.string()), -1);
                                          s->setDirty(true);
                                      }
                                      break;
                                  }
                                  case 3118: s->move(a->item, true); break;
                                  case 3119: s->move(a->item, false); break;
                                  default: break;
                                  }
                              }), a, +[](gpointer p, GClosure*) { delete static_cast<Act*>(p); }, GConnectFlags(0));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    };
    auto sep = [&] { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); };
    if (workspaceMenu || kind == Kind::Root) {
        add("WorkspaceMenu", 3122, "New Workspace");
        add("WorkspaceMenu", 3123, "Open Workspace");
        add("WorkspaceMenu", 3124, "Reload Workspace");
        sep();
        add("WorkspaceMenu", 3125, "Save");
        add("WorkspaceMenu", 3126, "Save As...");
        add("WorkspaceMenu", 3127, "Save a Copy As...");
        sep();
        add("WorkspaceMenu", 3121, "Add New Project");
        sep();
        add("WorkspaceMenu", 3128, "Find in Projects...");
    } else if (kind == Kind::Project || kind == Kind::Folder) {
        const char* m = kind == Kind::Project ? "ProjectMenu" : "FolderMenu";
        add(m, 3118, "Move Up");
        add(m, 3119, "Move Down");
        sep();
        add(m, 3111, "Rename");
        add(m, 3112, "Add Folder");
        add(m, 3113, "Add Files...");
        add(m, 3117, "Add Files from Directory...");
        add(m, 3114, "Remove");
    } else {
        add("FileMenu", 3118, "Move Up");
        add("FileMenu", 3119, "Move Down");
        sep();
        add("FileMenu", 3111, "Rename");
        add("FileMenu", 3115, "Remove");
        add("FileMenu", 3116, "Modify File Path");
    }
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* sh, gpointer) {
                         g_idle_add(+[](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, sh);
                     }), nullptr);
    return menu;
}

void ProjectPanel::popup(GdkEvent* ev, bool)
{
    GtkWidget* menu = buildMenu(false);
    gtk_menu_attach_to_widget(GTK_MENU(menu), tree_, nullptr);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), ev);
}

std::string ProjectPanel::dump() const
{
    std::string out;
    std::function<void(GtkTreeIter*, int)> walk = [&](GtkTreeIter* parent, int depth) {
        GtkTreeIter it;
        if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(store_), &it, parent))
            return;
        do {
            gchar* l = nullptr;
            gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, kColLabel, &l, -1);
            out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + (l ? l : "") + "\n";
            g_free(l);
            walk(&it, depth + 1);
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store_), &it));
    };
    walk(nullptr, 0);
    return out;
}

} // namespace npp::ui
