#include "ui/DockManager.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace npp::ui {

namespace {

constexpr const char* kGroup = "npp-dock";
constexpr const char* kPanelKey = "npp-dock-panel";
constexpr const char* kPendingKey = "npp-dock-pending-size";

int idx(DockManager::Area a) { return static_cast<int>(a); }

} // namespace

DockManager::DockManager(GtkWindow* main, GtkWidget* center) : main_(main)
{
    for (int a = 0; a < 4; ++a)
        sides_[a] = makeNotebook();
    GtkWidget* paneL = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget* paneR = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget* paneT = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    GtkWidget* paneB = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(paneL), sides_[idx(Area::Left)], FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paneL), paneR, TRUE, FALSE);
    gtk_paned_pack1(GTK_PANED(paneR), paneT, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paneR), sides_[idx(Area::Right)], FALSE, FALSE);
    gtk_paned_pack1(GTK_PANED(paneT), sides_[idx(Area::Top)], FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paneT), paneB, TRUE, FALSE);
    gtk_paned_pack1(GTK_PANED(paneB), center, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paneB), sides_[idx(Area::Bottom)], FALSE, FALSE);
    panes_[idx(Area::Left)] = paneL;
    panes_[idx(Area::Right)] = paneR;
    panes_[idx(Area::Top)] = paneT;
    panes_[idx(Area::Bottom)] = paneB;
    root_ = paneL;
    g_object_ref_sink(root_);
    // Destroying the main window destroys the docked notebooks too, which
    // drop their pages one by one: stop reacting before the panels' widgets
    // go (and disconnect while the notebooks and panes still exist).
    g_signal_connect(root_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer self) {
                         auto* d = static_cast<DockManager*>(self);
                         d->gone_ = true;
                         for (GtkWidget* w : d->sides_)
                             g_signal_handlers_disconnect_by_data(w, d);
                         for (GtkWidget* w : d->panes_)
                             g_signal_handlers_disconnect_by_data(w, d);
                     }), this);

    for (int a = 0; a < 4; ++a) {
        GtkWidget* pane = panes_[a];
        g_object_set_data(G_OBJECT(pane), "npp-dock-area", GINT_TO_POINTER(a));
        // A side that becomes visible gets its remembered size once laid out.
        g_signal_connect_after(pane, "size-allocate", G_CALLBACK(+[](GtkWidget* w, GdkRectangle*, gpointer self) {
                                   auto* d = static_cast<DockManager*>(self);
                                   const int a = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "npp-dock-area"));
                                   if (g_object_get_data(G_OBJECT(w), kPendingKey)) {
                                       g_object_set_data(G_OBJECT(w), kPendingKey, nullptr);
                                       d->applySize(static_cast<Area>(a));
                                   }
                               }), this);
        // ... and the user's dragging is remembered.
        g_signal_connect(pane, "notify::position", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer self) {
                             auto* d = static_cast<DockManager*>(self);
                             GtkWidget* w = GTK_WIDGET(o);
                             const int a = GPOINTER_TO_INT(g_object_get_data(o, "npp-dock-area"));
                             if (d->restoring_ || g_object_get_data(o, kPendingKey) || !gtk_widget_get_visible(d->sides_[a]))
                                 return;
                             const int pos = gtk_paned_get_position(GTK_PANED(w));
                             const bool horizontal = a == idx(Area::Left) || a == idx(Area::Right);
                             const int total = horizontal ? gtk_widget_get_allocated_width(w) : gtk_widget_get_allocated_height(w);
                             const int size = (a == idx(Area::Left) || a == idx(Area::Top)) ? pos : total - pos;
                             if (size > 20 && total > 1)
                                 d->sizes_[static_cast<std::size_t>(a)] = size;
                         }), this);
    }
    for (GtkWidget* nb : sides_)
        gtk_widget_set_no_show_all(nb, TRUE);
}

DockManager::~DockManager()
{
    for (GtkWidget* nb : floating_) {
        g_signal_handlers_disconnect_by_data(nb, this); // its pages go with it
        if (GtkWidget* win = floatingWindow(nb))
            gtk_widget_destroy(win);
    }
    if (!gone_) {
        g_signal_handlers_disconnect_by_data(root_, this);
        for (GtkWidget* nb : sides_)
            g_signal_handlers_disconnect_by_data(nb, this);
        for (GtkWidget* p : panes_)
            g_signal_handlers_disconnect_by_data(p, this);
    }
    gtk_widget_destroy(root_);
    g_object_unref(root_);
}

GtkWidget* DockManager::makeNotebook()
{
    GtkWidget* nb = gtk_notebook_new();
    gtk_notebook_set_group_name(GTK_NOTEBOOK(nb), kGroup);
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), GTK_POS_BOTTOM); // upstream: tabs under the panels
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(nb), FALSE);
    g_signal_connect(nb, "page-added", G_CALLBACK(&DockManager::onPageAdded), this);
    g_signal_connect(nb, "page-removed", G_CALLBACK(&DockManager::onPageRemoved), this);
    g_signal_connect(nb, "create-window", G_CALLBACK(&DockManager::onCreateWindow), this);
    return nb;
}

GtkWidget* DockManager::floatingWindow(GtkWidget* nb) const
{
    GtkWidget* top = gtk_widget_get_toplevel(nb);
    return top && GTK_IS_WINDOW(top) && top != GTK_WIDGET(main_) ? top : nullptr;
}

GtkWidget* DockManager::newFloatingNotebook(int x, int y, int w, int h)
{
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(win), main_);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_default_size(GTK_WINDOW(win), w > 0 ? w : 320, h > 0 ? h : 480);
    if (x >= 0 && y >= 0)
        gtk_window_move(GTK_WINDOW(win), x, y);
    GtkWidget* nb = makeNotebook();
    gtk_widget_set_no_show_all(nb, TRUE);
    gtk_container_add(GTK_CONTAINER(win), nb);
    // Closing a floating window hides its panels; they stay floating.
    g_signal_connect(win, "delete-event", G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer self) -> gboolean {
                         auto* d = static_cast<DockManager*>(self);
                         GtkWidget* nb = gtk_bin_get_child(GTK_BIN(w));
                         for (auto& p : d->panels_)
                             if (p->shown && d->notebookOf(*p) == nb)
                                 d->setShown(*p, false);
                         return TRUE;
                     }), this);
    floating_.push_back(nb);
    return nb;
}

DockManager::Area DockManager::areaOfNotebook(GtkWidget* nb) const
{
    for (int a = 0; a < 4; ++a)
        if (sides_[a] == nb)
            return static_cast<Area>(a);
    return Area::Float;
}

GtkWidget* DockManager::notebookOf(const Panel& p) const
{
    return gtk_widget_get_parent(p.page);
}

DockManager::Panel* DockManager::panelForPage(GtkWidget* page)
{
    return static_cast<Panel*>(g_object_get_data(G_OBJECT(page), kPanelKey));
}

DockManager::Panel* DockManager::find(const std::string& id)
{
    for (auto& p : panels_)
        if (p->id == id)
            return p.get();
    return nullptr;
}

const DockManager::Panel* DockManager::find(const std::string& id) const
{
    for (const auto& p : panels_)
        if (p->id == id)
            return p.get();
    return nullptr;
}

void DockManager::setMenuLabels(std::array<std::string, 5> labels, std::string close)
{
    menuLabels_ = std::move(labels);
    closeLabel_ = std::move(close);
}

void DockManager::add(const std::string& id, const std::string& title, GtkWidget* content, Area area)
{
    auto owned = std::make_unique<Panel>();
    Panel& p = *owned;
    p.id = id;
    p.title = title;
    p.area = area;

    // Caption bar, as upstream's DockingCont: title, then a menu and close.
    p.page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data(G_OBJECT(p.page), kPanelKey, &p);
    p.caption = gtk_label_new(title.c_str());
    gtk_label_set_xalign(GTK_LABEL(p.caption), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p.caption), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(p.caption, 6);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(bar), p.caption, TRUE, TRUE, 0);
    GtkWidget* more = gtk_button_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    for (GtkWidget* b : {more, close}) {
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_widget_set_focus_on_click(b, FALSE);
        gtk_box_pack_end(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    gtk_box_reorder_child(GTK_BOX(bar), close, -1);
    gtk_widget_set_tooltip_text(close, closeLabel_.c_str());
    g_object_set_data(G_OBJECT(close), kPanelKey, &p);
    g_object_set_data(G_OBJECT(more), kPanelKey, &p);
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer self) {
                         auto* d = static_cast<DockManager*>(self);
                         d->setShown(*static_cast<Panel*>(g_object_get_data(G_OBJECT(b), kPanelKey)), false);
                     }), this);
    g_signal_connect(more, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer self) {
                         auto* d = static_cast<DockManager*>(self);
                         Panel* panel = static_cast<Panel*>(g_object_get_data(G_OBJECT(b), kPanelKey));
                         GtkWidget* menu = gtk_menu_new();
                         for (int a = 0; a <= idx(Area::Float); ++a) {
                             GtkWidget* item = gtk_menu_item_new_with_label(d->menuLabels_[static_cast<std::size_t>(a)].c_str());
                             gtk_widget_set_sensitive(item, d->areaOf(panel->id) != static_cast<Area>(a));
                             g_object_set_data(G_OBJECT(item), "npp-dock-area", GINT_TO_POINTER(a));
                             g_object_set_data(G_OBJECT(item), kPanelKey, panel);
                             g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem* it, gpointer self2) {
                                                  auto* d2 = static_cast<DockManager*>(self2);
                                                  auto* pn = static_cast<Panel*>(g_object_get_data(G_OBJECT(it), kPanelKey));
                                                  d2->moveTo(pn->id, static_cast<Area>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "npp-dock-area"))));
                                              }), d);
                             gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
                         }
                         gtk_widget_show_all(menu);
                         gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(b), nullptr);
                         g_signal_connect(menu, "deactivate", G_CALLBACK(+[](GtkMenuShell* m, gpointer) {
                                              g_idle_add(+[](gpointer w) -> gboolean { gtk_widget_destroy(GTK_WIDGET(w)); return G_SOURCE_REMOVE; }, m);
                                          }), nullptr);
                         gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, nullptr);
                     }), this);
    gtk_box_pack_start(GTK_BOX(p.page), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p.page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_widget_set_vexpand(content, TRUE);
    gtk_box_pack_start(GTK_BOX(p.page), content, TRUE, TRUE, 0);
    gtk_widget_show_all(p.page);
    gtk_widget_hide(p.page); // panels start hidden
    p.tabLabel = gtk_label_new(title.c_str());
    gtk_widget_show(p.tabLabel);

    // A panel added after restoreState() (a plugin's) goes where it was.
    if (auto it = pending_.find(id); it != pending_.end()) {
        int a = 0, shown = 0, order = 0;
        GdkRectangle r{-1, -1, 320, 480};
        if (std::sscanf(it->second.c_str(), "%d,%d,%d,%d,%d,%d,%d", &a, &shown, &order, &r.x, &r.y, &r.width, &r.height) >= 3) {
            p.area = area = static_cast<Area>(std::clamp(a, 0, idx(Area::Float)));
            p.floatRect = r;
        }
        pending_.erase(it);
    }

    panels_.push_back(std::move(owned));
    GtkWidget* nb = area == Area::Float ? newFloatingNotebook(p.floatRect.x, p.floatRect.y, p.floatRect.width, p.floatRect.height)
                                        : sides_[idx(area)];
    place(p, nb);
}

bool DockManager::has(const std::string& id) const
{
    return find(id) != nullptr;
}

std::vector<std::string> DockManager::pendingShown() const
{
    std::vector<std::string> out;
    for (const auto& [id, v] : pending_) {
        int a = 0, shown = 0;
        if (std::sscanf(v.c_str(), "%d,%d", &a, &shown) == 2 && shown)
            out.push_back(id);
    }
    return out;
}

void DockManager::place(Panel& p, GtkWidget* nb)
{
    GtkWidget* old = gtk_widget_get_parent(p.page);
    if (old == nb)
        return;
    if (old) {
        g_object_ref(p.page);
        g_object_ref(p.tabLabel);
        gtk_container_remove(GTK_CONTAINER(old), p.page);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), p.page, p.tabLabel);
        g_object_unref(p.tabLabel);
        g_object_unref(p.page);
    } else {
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), p.page, p.tabLabel);
    }
}

void DockManager::onPageAdded(GtkNotebook* nb, GtkWidget* page, guint, gpointer self)
{
    auto* d = static_cast<DockManager*>(self);
    Panel* p = d->panelForPage(page);
    if (!p)
        return;
    gtk_notebook_set_tab_reorderable(nb, page, TRUE);
    gtk_notebook_set_tab_detachable(nb, page, TRUE);
    p->area = d->areaOfNotebook(GTK_WIDGET(nb));
    d->refreshArea(GTK_WIDGET(nb));
}

void DockManager::onPageRemoved(GtkNotebook* nb, GtkWidget*, guint, gpointer self)
{
    auto* d = static_cast<DockManager*>(self);
    GtkWidget* w = GTK_WIDGET(nb);
    auto it = std::find(d->floating_.begin(), d->floating_.end(), w);
    if (it != d->floating_.end() && gtk_notebook_get_n_pages(nb) == 0) {
        // An empty floating window goes away (after GTK is done with the move).
        d->floating_.erase(it);
        if (GtkWidget* win = d->floatingWindow(w)) {
            g_signal_handlers_disconnect_by_data(w, d);
            g_idle_add(+[](gpointer p) -> gboolean { gtk_widget_destroy(GTK_WIDGET(p)); return G_SOURCE_REMOVE; }, win);
        }
        return;
    }
    d->refreshArea(w);
}

GtkNotebook* DockManager::onCreateWindow(GtkNotebook*, GtkWidget* page, gint x, gint y, gpointer self)
{
    auto* d = static_cast<DockManager*>(self);
    Panel* p = d->panelForPage(page);
    const int w = p ? std::max(gtk_widget_get_allocated_width(page), 200) : 320;
    const int h = p ? std::max(gtk_widget_get_allocated_height(page), 200) : 480;
    return GTK_NOTEBOOK(d->newFloatingNotebook(x, y, w, h));
}

void DockManager::refreshArea(GtkWidget* nb)
{
    int shown = 0;
    for (auto& p : panels_)
        if (p->shown && notebookOf(*p) == nb)
            ++shown;
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nb), shown > 1);
    const Area a = areaOfNotebook(nb);
    if (a == Area::Float) {
        if (GtkWidget* win = floatingWindow(nb)) {
            if (shown) {
                gtk_widget_show(nb);
                gtk_widget_show(win);
            } else {
                gtk_widget_hide(win);
            }
        }
        return;
    }
    const bool was = gtk_widget_get_visible(nb);
    gtk_widget_set_visible(nb, shown > 0);
    if (shown && !was) {
        g_object_set_data(G_OBJECT(panes_[idx(a)]), kPendingKey, GINT_TO_POINTER(1));
        gtk_widget_queue_resize(panes_[idx(a)]);
    }
}

void DockManager::refreshAll()
{
    for (GtkWidget* nb : sides_)
        refreshArea(nb);
    for (GtkWidget* nb : std::vector<GtkWidget*>(floating_))
        refreshArea(nb);
}

void DockManager::applySize(Area a)
{
    GtkWidget* pane = panes_[idx(a)];
    const bool horizontal = a == Area::Left || a == Area::Right;
    const int total = horizontal ? gtk_widget_get_allocated_width(pane) : gtk_widget_get_allocated_height(pane);
    if (total <= 1)
        return;
    const int size = std::clamp(sizes_[static_cast<std::size_t>(idx(a))], 40, std::max(40, total - 80));
    restoring_ = true;
    gtk_paned_set_position(GTK_PANED(pane), (a == Area::Left || a == Area::Top) ? size : total - size);
    restoring_ = false;
}

void DockManager::setShown(Panel& p, bool shown)
{
    const bool changed = p.shown != shown;
    p.shown = shown;
    gtk_widget_set_visible(p.page, shown);
    GtkWidget* nb = notebookOf(p);
    refreshArea(nb);
    if (shown) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), gtk_notebook_page_num(GTK_NOTEBOOK(nb), p.page));
        if (GtkWidget* win = floatingWindow(nb))
            gtk_window_present(GTK_WINDOW(win));
    }
    if (changed && onShownChanged)
        onShownChanged(p.id, shown);
}

void DockManager::show(const std::string& id, bool focus)
{
    Panel* p = find(id);
    if (!p)
        return;
    setShown(*p, true);
    if (focus)
        gtk_widget_child_focus(p->page, GTK_DIR_TAB_FORWARD);
}

void DockManager::hide(const std::string& id)
{
    if (Panel* p = find(id))
        setShown(*p, false);
}

void DockManager::toggle(const std::string& id)
{
    if (Panel* p = find(id))
        setShown(*p, !p->shown);
}

bool DockManager::isShown(const std::string& id) const
{
    const Panel* p = find(id);
    return p && p->shown;
}

DockManager::Area DockManager::areaOf(const std::string& id) const
{
    const Panel* p = find(id);
    return p ? p->area : Area::Bottom;
}

void DockManager::setTitle(const std::string& id, const std::string& title)
{
    if (Panel* p = find(id)) {
        p->title = title;
        gtk_label_set_text(GTK_LABEL(p->caption), title.c_str());
        gtk_label_set_text(GTK_LABEL(p->tabLabel), title.c_str());
    }
}

void DockManager::moveTo(const std::string& id, Area area)
{
    Panel* p = find(id);
    if (!p || p->area == area)
        return;
    GtkWidget* nb = area == Area::Float ? newFloatingNotebook(p->floatRect.x, p->floatRect.y, p->floatRect.width, p->floatRect.height)
                                        : sides_[idx(area)];
    place(*p, nb);
    if (p->shown)
        setShown(*p, true);
    refreshAll();
}

std::map<std::string, std::string> DockManager::saveState() const
{
    std::map<std::string, std::string> out;
    for (const auto& p : panels_) {
        GtkWidget* nb = gtk_widget_get_parent(p->page);
        GdkRectangle r = p->floatRect;
        if (p->area == Area::Float)
            if (GtkWidget* win = floatingWindow(nb)) {
                gtk_window_get_position(GTK_WINDOW(win), &r.x, &r.y);
                gtk_window_get_size(GTK_WINDOW(win), &r.width, &r.height);
            }
        char buf[128];
        std::snprintf(buf, sizeof buf, "%d,%d,%d,%d,%d,%d,%d", idx(p->area), p->shown ? 1 : 0,
                      gtk_notebook_page_num(GTK_NOTEBOOK(nb), p->page), r.x, r.y, r.width, r.height);
        out[p->id] = buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d,%d,%d,%d", sizes_[0], sizes_[1], sizes_[2], sizes_[3]);
    out["sizes"] = buf;
    // Panels of plugins that were not loaded (or not opened) this time.
    for (const auto& [id, v] : pending_)
        out.try_emplace(id, v);
    return out;
}

void DockManager::restoreState(const std::map<std::string, std::string>& state)
{
    if (auto it = state.find("sizes"); it != state.end())
        std::sscanf(it->second.c_str(), "%d,%d,%d,%d", &sizes_[0], &sizes_[1], &sizes_[2], &sizes_[3]);
    struct Placed {
        Panel* p;
        int order;
        bool shown;
    };
    std::vector<Placed> placed;
    pending_.clear();
    for (const auto& [id, v] : state)
        if (id != "sizes" && !find(id))
            pending_[id] = v;
    for (auto& p : panels_) {
        auto it = state.find(p->id);
        if (it == state.end())
            continue;
        int area = 0, shown = 0, order = 0;
        GdkRectangle r{-1, -1, 320, 480};
        if (std::sscanf(it->second.c_str(), "%d,%d,%d,%d,%d,%d,%d", &area, &shown, &order, &r.x, &r.y, &r.width, &r.height) < 3)
            continue;
        p->floatRect = r;
        moveTo(p->id, static_cast<Area>(std::clamp(area, 0, idx(Area::Float))));
        placed.push_back({p.get(), order, shown != 0});
    }
    std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.order < b.order; });
    for (const Placed& pl : placed) {
        GtkWidget* nb = notebookOf(*pl.p);
        gtk_notebook_reorder_child(GTK_NOTEBOOK(nb), pl.p->page, pl.order);
    }
    for (const Placed& pl : placed)
        if (pl.shown)
            setShown(*pl.p, true);
}

} // namespace npp::ui
