// Docking panels — the port's counterpart of upstream WinControls/DockingWnd
// (DockingManager, DockingCont). PORTING.md §2.2 left the choice open; this
// is the GtkPaned-based dock it describes:
//
//   ┌──────┬─────────────────────┬───────┐
//   │      │        Top          │       │
//   │ Left ├─────────────────────┤ Right │
//   │      │ center (editors)    │       │
//   │      ├─────────────────────┤       │
//   │      │       Bottom        │       │
//   └──────┴─────────────────────┴───────┘
//
// Each side is a GtkNotebook. All of them share one tab group, so GTK's own
// tab drag-and-drop moves a panel to another side, and dropping it outside
// the window makes it float in a window of its own, as upstream's gripper
// does. A panel can also be moved from the menu in its caption bar. A side
// with no visible panel takes no room. Upstream keeps the docking layout in
// config.xml; the port stores it in settings.ini through saveState().
#pragma once

#include <gtk/gtk.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace npp::ui {

class DockManager {
public:
    enum class Area { Left = 0, Right, Top, Bottom, Float };

    DockManager(GtkWindow* main, GtkWidget* center);
    ~DockManager();
    // False once the main window has destroyed the docked widgets (it does
    // so before the dock goes): their layout can no longer be read.
    bool alive() const { return !gone_; }
    DockManager(const DockManager&) = delete;
    DockManager& operator=(const DockManager&) = delete;

    GtkWidget* widget() const { return root_; }

    // Adds a hidden panel. `content` is owned by the dock from now on.
    // A panel restoreState() knew is placed where it was saved instead.
    void add(const std::string& id, const std::string& title, GtkWidget* content, Area area);
    bool has(const std::string& id) const;
    void setTitle(const std::string& id, const std::string& title);
    void show(const std::string& id, bool focus = true);
    void hide(const std::string& id);
    void toggle(const std::string& id);
    bool isShown(const std::string& id) const;
    Area areaOf(const std::string& id) const;
    void moveTo(const std::string& id, Area area);
    // Translations for the caption menu ("Dock to Left" ...), set before add().
    void setMenuLabels(std::array<std::string, 5> labels, std::string close);

    // Called whenever a panel is shown or hidden, by the user or by code.
    std::function<void(const std::string& id, bool shown)> onShownChanged;

    // "id" -> "area,shown,order,x,y,w,h"; "sizes" -> "left,right,top,bottom".
    std::map<std::string, std::string> saveState() const;
    // State of panels not added yet is kept: applied when they are added,
    // saved again if they never are (plugins that were not loaded).
    void restoreState(const std::map<std::string, std::string>& state);
    // Panels not added yet that were shown when the state was saved.
    std::vector<std::string> pendingShown() const;

private:
    struct Panel {
        std::string id;
        std::string title;
        GtkWidget* page = nullptr;    // caption + content, the notebook page
        GtkWidget* tabLabel = nullptr;
        GtkWidget* caption = nullptr;
        Area area = Area::Bottom;
        bool shown = false;
        GdkRectangle floatRect{-1, -1, 320, 480};
    };
    GtkWidget* makeNotebook();
    GtkWidget* notebookOf(const Panel& p) const;
    GtkWidget* floatingWindow(GtkWidget* notebook) const;
    GtkWidget* newFloatingNotebook(int x, int y, int w, int h);
    Panel* panelForPage(GtkWidget* page);
    Panel* find(const std::string& id);
    const Panel* find(const std::string& id) const;
    void place(Panel& p, GtkWidget* notebook);
    void refreshArea(GtkWidget* notebook);
    void refreshAll();
    void applySize(Area a);
    Area areaOfNotebook(GtkWidget* nb) const;
    void setShown(Panel& p, bool shown);

    static void onPageAdded(GtkNotebook* nb, GtkWidget* page, guint, gpointer self);
    static void onPageRemoved(GtkNotebook* nb, GtkWidget* page, guint, gpointer self);
    static GtkNotebook* onCreateWindow(GtkNotebook* nb, GtkWidget* page, gint x, gint y, gpointer self);

    GtkWindow* main_;
    GtkWidget* root_ = nullptr;
    GtkWidget* panes_[4] = {};        // paned that holds each side
    GtkWidget* sides_[4] = {};        // the notebooks
    std::vector<GtkWidget*> floating_; // floating notebooks
    std::vector<std::unique_ptr<Panel>> panels_; // stable addresses for signal handlers
    std::array<int, 4> sizes_{220, 260, 160, 200};
    std::array<std::string, 5> menuLabels_{"Dock to Left", "Dock to Right", "Dock to Top", "Dock to Bottom", "Float"};
    std::string closeLabel_ = "Close";
    bool restoring_ = false;
    bool gone_ = false; // the main window destroyed the docked widgets
    std::map<std::string, std::string> pending_; // saved state of panels not added yet
};

} // namespace npp::ui
