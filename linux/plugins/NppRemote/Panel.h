// NppRemote — the docked panel: profile menu, remote folder tree, the
// commands on it and a log. Files open from a local cache
// (<cache>/<profile>/<remote path>); saving one uploads it back.
#pragma once

#include "Profiles.h"
#include "Session.h"

#include <windows.h>
#include "NppGtk.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nppremote {

class Panel {
public:
    Panel(const NppData& npp, std::string settingsFile, std::string cacheRoot, int panelCommand);
    ~Panel();

    // Docks and shows the panel (registering it the first time), or hides it.
    void setShown(bool shown);
    bool shown() const { return shown_; }
    // The user closed it with the dock's own button.
    void closedByUser() { shown_ = false; }

    void editProfiles();
    void connectTo(const std::string& profileName);
    void disconnect();
    // The current document: back to where it came from when it is a cached
    // remote file, else into the selected remote folder.
    void uploadCurrent();
    // Notepad++ saved `path`: upload it when it is in the cache.
    void fileSaved(const std::string& path);
    // NPPN_SHUTDOWN: logs out; the widgets go with the window.
    void shutdown();

    std::function<void(bool shown)> shownChanged; // menu check mark

private:
    void build();
    void updateButtons();
    void updateTitle();
    void log(LogKind kind, const std::string& text);

    // ---- tree ----
    void setRoot(const std::string& path);
    void loadDir(const std::string& path);
    bool findRow(const std::string& path, GtkTreeIter& out);
    bool selected(std::string& path, int& kind);
    std::string selectedDir();
    void refreshDir(const std::string& dir);
    void fill(GtkTreeIter* parent, std::vector<Entry> entries);
    void popupMenu(GdkEventButton* ev);

    // ---- commands ----
    void openRemote(const std::string& remote);
    void downloadTo(const std::string& remote);
    void refreshLocalCopy(const std::string& remote);
    void uploadFile(const std::string& local, const std::string& remote, bool refreshAfter);
    void newFile();
    void newFolder();
    void renameSelected();
    void deleteSelected();
    void showProfileMenu(GtkWidget* anchor);

    // ---- helpers ----
    std::string cacheFile(const std::string& remote) const;
    const Profile* profileByFolder(const std::string& folder) const;
    bool ensureSession(const Profile& p);
    GtkWindow* mainWindow() const;
    void showError(const std::string& what, const std::string& why) const;
    bool askText(const std::string& title, const std::string& label, std::string& value) const;
    bool askSecret(const std::string& prompt, std::string& answer) const;
    bool confirm(const std::string& question, const std::string& yes) const;
    std::string currentFilePath() const;
    void saveSettings();

    NppData npp_;
    std::string settingsFile_;
    std::string cacheRoot_;
    int panelCommand_;
    Settings settings_;
    std::unique_ptr<Session> session_;
    bool connected_ = false;
    bool busy_ = false;
    bool shown_ = false;
    bool registered_ = false;
    bool shutDown_ = false;
    std::string root_;
    std::string home_; // the login folder, for "~"

    GtkWidget* box_ = nullptr;
    GtkWidget* connectButton_ = nullptr;
    std::vector<GtkWidget*> needConnection_; // buttons for a live session
    GtkWidget* stopButton_ = nullptr;
    GtkWidget* spinner_ = nullptr;
    GtkWidget* pathEntry_ = nullptr;
    GtkWidget* paned_ = nullptr;
    GtkWidget* logBox_ = nullptr;
    GtkWidget* logView_ = nullptr;
    GtkTreeStore* store_ = nullptr;
    GtkWidget* tree_ = nullptr;
    DockedWidgetData dock_{};
    std::wstring title_ = L"Remote Files";
    wchar_t addInfo_[128] = {}; // the dock reads it through its pointer
};

} // namespace nppremote
