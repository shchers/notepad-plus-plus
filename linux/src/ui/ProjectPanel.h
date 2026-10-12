// Project Panels 1-3 — upstream ProjectPanel: a workspace of projects,
// folders and files kept in a .workspace file (core/Workspace, upstream
// format). The Workspace and Edit menus, and the context menus, follow
// upstream: new / open / reload / save workspace, add project, folder,
// files or a whole directory, rename in place, remove, move up / down,
// modify a file's path, Find in Projects. A changed workspace is offered
// for saving on exit.
#pragma once

#include "core/Translator.h"
#include "core/Workspace.h"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace npp::ui {

struct ProjectPanelHost {
    std::function<void(const std::filesystem::path&)> openFile;
    std::function<void(const std::vector<std::filesystem::path>&)> findInProjects;
    // Yes / No / Cancel: 1 / 0 / -1.
    std::function<int(const std::string& primary, const std::string& secondary, bool cancellable)> ask;
    std::function<void(const std::string& primary, const std::string& secondary)> error;
};

class ProjectPanel {
public:
    ProjectPanel(int number, const core::Translator& tr, ProjectPanelHost host);
    ~ProjectPanel();

    GtkWidget* widget() const { return box_; }
    std::string title() const; // "Project Panel 1"
    int number() const { return number_; }

    bool openWorkspace(const std::filesystem::path& file);
    bool saveWorkspace(const std::filesystem::path& file = {}); // empty: the current file (or Save As)
    void newWorkspace();
    const std::filesystem::path& workspaceFile() const { return file_; }
    bool isDirty() const { return dirty_; }
    // Asks to save a changed workspace; false = cancelled.
    bool confirmClose();

    // Building blocks of the menus (also used by tests). `parent` is a tree
    // path string ("0:1"); empty = the workspace root.
    std::string addProject(const std::string& name);
    std::string addFolder(const std::string& parent, const std::string& name);
    void addFiles(const std::string& parent, const std::vector<std::filesystem::path>& files);
    void addDirectory(const std::string& parent, const std::filesystem::path& dir);
    void rename(const std::string& item, const std::string& name);
    void remove(const std::string& item);
    void move(const std::string& item, bool up);
    core::Workspace workspace() const;
    std::string dump() const;

private:
    enum class Kind { Root = -1, Project = 0, Folder = 1, File = 2 };
    void setDirty(bool d);
    void load(const core::Workspace& ws);
    void addNodes(GtkTreeIter* parent, const std::vector<core::WorkspaceNode>& nodes);
    void readNodes(GtkTreeIter* parent, std::vector<core::WorkspaceNode>& out) const;
    bool iterFor(const std::string& path, GtkTreeIter* out) const;
    Kind kindOf(GtkTreeIter* it) const;
    void startRename(GtkTreeIter* it);
    void popup(GdkEvent* ev, bool editMenu);
    GtkWidget* buildMenu(bool workspaceMenu);
    std::string label(const char* menu, int id, const char* fallback) const;
    std::string selected() const;
    std::vector<std::filesystem::path> chooseFiles() const;
    std::filesystem::path chooseFolder() const;
    std::filesystem::path chooseSave(const char* title) const;

    int number_;
    const core::Translator& tr_;
    ProjectPanelHost host_;
    std::filesystem::path file_;
    bool dirty_ = false;
    GtkWidget* box_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkTreeStore* store_ = nullptr;
    GtkCellRenderer* text_ = nullptr;
    GtkTreeViewColumn* column_ = nullptr;
};

} // namespace npp::ui
