// Folder as Workspace — upstream FileBrowser: folders shown as trees, loaded
// as they are expanded and refreshed when they change on disk (GFileMonitor,
// as upstream uses ReadDirectoryChangesW). Folders first, then files, by
// name. Double-click opens a file; the context menus follow upstream's (Copy
// path, Find in Files, file manager and terminal here, Run by system). The
// roots, expanded folders and selection are kept in session.xml.
#pragma once

#include "core/Session.h"
#include "core/Translator.h"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace npp::ui {

struct FolderPanelHost {
    std::function<void(const std::filesystem::path&)> openFile;
    std::function<void(const std::filesystem::path& dir)> findInFiles;
    std::function<void(const std::filesystem::path& dir)> openTerminal;
    std::function<void(const std::string& uri)> openUri;
    std::function<std::filesystem::path()> currentFile; // "Locate current file"
    std::function<std::filesystem::path()> chooseFolder; // "Add"
    // Preferences > MISC.: "Allow loading symlinks in Folder as Workspace panel".
    std::function<bool()> followSymlinks;
};

class FolderPanel {
public:
    FolderPanel(const core::Translator& tr, FolderPanelHost host);
    ~FolderPanel();

    GtkWidget* widget() const { return box_; }
    void addRoot(const std::filesystem::path& dir);
    void removeRoot(const std::filesystem::path& dir);
    void clear();
    std::vector<std::filesystem::path> roots() const;
    // Expands the folders down to `file` and selects it. False if not under a root.
    bool locate(const std::filesystem::path& file);
    void expandAll();
    void collapseAll();
    std::filesystem::path selectedPath() const;

    void saveTo(core::Session& s) const;
    void restoreFrom(const core::Session& s);
    // The visible tree, indented, for tests.
    std::string dump() const;
    // Re-reads a listed folder (also done automatically on changes).
    void refresh(const std::filesystem::path& dir);

private:
    bool findRow(const std::filesystem::path& p, GtkTreeIter* out) const;
    void fill(GtkTreeIter* parent, const std::filesystem::path& dir);
    void ensureLoaded(GtkTreeIter* it);
    void watch(const std::filesystem::path& dir);
    void popupMenu(GdkEventButton* ev);
    std::string text(int id, const char* fallback) const;

    const core::Translator& tr_;
    FolderPanelHost host_;
    GtkWidget* box_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkTreeStore* store_ = nullptr;
    std::map<std::string, GFileMonitor*> monitors_; // per listed folder
};

} // namespace npp::ui
