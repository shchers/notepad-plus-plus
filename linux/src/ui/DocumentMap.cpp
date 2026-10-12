#include "ui/DocumentMap.h"

#include <algorithm>

namespace npp::ui {

DocumentMap::DocumentMap(std::function<void(EditorView&, EditorView&)> restyle) : restyle_(std::move(restyle))
{
    // Upstream: zoom -10, no margins, no caret, no scroll bars.
    map_.showLineNumbers = false;
    for (int m = 0; m < 5; ++m)
        map_.execute(SCI_SETMARGINWIDTHN, static_cast<uptr_t>(m), 0);
    map_.execute(SCI_SETZOOM, static_cast<uptr_t>(-10));
    map_.execute(SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
    map_.execute(SCI_SETCARETLINEVISIBLE, 0);
    map_.execute(SCI_SETHSCROLLBAR, 0);
    map_.execute(SCI_SETVSCROLLBAR, 0);
    map_.execute(SCI_SETCURSOR, SC_CURSORARROW);
    gtk_widget_set_can_focus(map_.widget(), FALSE);

    overlay_ = gtk_overlay_new();
    g_object_ref_sink(overlay_);
    gtk_container_add(GTK_CONTAINER(overlay_), map_.widget());
    zoneArea_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(zoneArea_, TRUE);
    gtk_widget_set_vexpand(zoneArea_, TRUE);
    gtk_widget_add_events(zoneArea_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), zoneArea_);
    g_signal_connect(zoneArea_, "draw", G_CALLBACK(+[](GtkWidget* w, cairo_t* cr, gpointer p) -> gboolean {
                         auto* self = static_cast<DocumentMap*>(p);
                         if (!self->source_ || self->zoneH_ <= 0)
                             return FALSE;
                         // A translucent band, like upstream's view zone.
                         GtkStyleContext* ctx = gtk_widget_get_style_context(w);
                         GdkRGBA c;
                         if (!gtk_style_context_lookup_color(ctx, "theme_selected_bg_color", &c))
                             c.red = 0.2;
                             c.green = 0.4;
                             c.blue = 0.8;
                             c.alpha = 1;
                         const int width = gtk_widget_get_allocated_width(w);
                         cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.25);
                         cairo_rectangle(cr, 0, self->zoneY_, width, self->zoneH_);
                         cairo_fill(cr);
                         cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.6);
                         cairo_rectangle(cr, 0.5, self->zoneY_ + 0.5, width - 1, self->zoneH_ - 1);
                         cairo_set_line_width(cr, 1);
                         cairo_stroke(cr);
                         return FALSE;
                     }), this);
    g_signal_connect(zoneArea_, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<DocumentMap*>(p);
                         if (ev->button != 1)
                             return FALSE;
                         self->dragging_ = true;
                         self->scrollTo(static_cast<int>(ev->y));
                         return TRUE;
                     }), this);
    g_signal_connect(zoneArea_, "button-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton*, gpointer p) -> gboolean {
                         static_cast<DocumentMap*>(p)->dragging_ = false;
                         return TRUE;
                     }), this);
    g_signal_connect(zoneArea_, "motion-notify-event", G_CALLBACK(+[](GtkWidget*, GdkEventMotion* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<DocumentMap*>(p);
                         if (self->dragging_)
                             self->scrollTo(static_cast<int>(ev->y));
                         return TRUE;
                     }), this);
    g_signal_connect(zoneArea_, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<DocumentMap*>(p);
                         if (!self->source_)
                             return FALSE;
                         // The wheel scrolls the editor, as upstream.
                         double dx = 0;
                         double dy = 0;
                         int lines = 0;
                         if (ev->direction == GDK_SCROLL_UP)
                             lines = -3;
                         else if (ev->direction == GDK_SCROLL_DOWN)
                             lines = 3;
                         else if (gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(ev), &dx, &dy))
                             lines = static_cast<int>(dy * 3);
                         self->source_->execute(SCI_LINESCROLL, 0, lines);
                         self->sync();
                         return TRUE;
                     }), this);
    g_signal_connect_after(map_.widget(), "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer p) {
                               static_cast<DocumentMap*>(p)->sync();
                           }), this);
}

DocumentMap::~DocumentMap()
{
    g_signal_handlers_disconnect_by_data(map_.widget(), this);
    gtk_widget_destroy(overlay_);
    g_object_unref(overlay_);
}

void DocumentMap::attach(EditorView* source)
{
    source_ = source;
    if (!source) {
        map_.execute(SCI_SETDOCPOINTER, 0, 0); // an empty document of its own
        gtk_widget_queue_draw(zoneArea_);
        return;
    }
    map_.execute(SCI_SETDOCPOINTER, 0, source->execute(SCI_GETDOCPOINTER));
    if (restyle_)
        restyle_(map_, *source);
    map_.execute(SCI_SETZOOM, static_cast<uptr_t>(-10));
    for (int m = 0; m < 5; ++m)
        map_.execute(SCI_SETMARGINWIDTHN, static_cast<uptr_t>(m), 0);
    sync();
}

void DocumentMap::sync()
{
    if (!source_) {
        zoneH_ = 0;
        gtk_widget_queue_draw(zoneArea_);
        return;
    }
    EditorView& s = *source_;
    // Same wrapping as the editor, so the lines correspond (upstream wrapMap).
    if (map_.execute(SCI_GETWRAPMODE) != s.execute(SCI_GETWRAPMODE))
        map_.execute(SCI_SETWRAPMODE, static_cast<uptr_t>(s.execute(SCI_GETWRAPMODE)));
    const sptr_t firstDisp = s.execute(SCI_GETFIRSTVISIBLELINE);
    const sptr_t onScreen = std::max<sptr_t>(1, s.execute(SCI_LINESONSCREEN));
    const sptr_t firstDoc = s.execute(SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>(firstDisp));
    const sptr_t lastDoc = s.execute(SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>(firstDisp + onScreen));
    // Keep the zone centred in the map where possible.
    const sptr_t mapFirstZone = map_.execute(SCI_VISIBLEFROMDOCLINE, static_cast<uptr_t>(firstDoc));
    const sptr_t mapLastZone = map_.execute(SCI_VISIBLEFROMDOCLINE, static_cast<uptr_t>(lastDoc));
    const sptr_t mapOnScreen = std::max<sptr_t>(1, map_.execute(SCI_LINESONSCREEN));
    const sptr_t zoneLines = std::max<sptr_t>(1, mapLastZone - mapFirstZone);
    map_.execute(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(std::max<sptr_t>(0, mapFirstZone - (mapOnScreen - zoneLines) / 2)));
    const int lineH = static_cast<int>(map_.execute(SCI_TEXTHEIGHT, 0));
    zoneY_ = static_cast<int>((mapFirstZone - map_.execute(SCI_GETFIRSTVISIBLELINE)) * lineH);
    zoneH_ = static_cast<int>(zoneLines * lineH);
    gtk_widget_queue_draw(zoneArea_);
}

void DocumentMap::scrollTo(int mapY)
{
    if (!source_)
        return;
    EditorView& s = *source_;
    const int lineH = std::max<int>(1, static_cast<int>(map_.execute(SCI_TEXTHEIGHT, 0)));
    const sptr_t mapLine = map_.execute(SCI_GETFIRSTVISIBLELINE) + mapY / lineH;
    const sptr_t docLine = map_.execute(SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>(mapLine));
    // Centre that line in the editor (upstream DocumentMap::scrollMap).
    const sptr_t disp = s.execute(SCI_VISIBLEFROMDOCLINE, static_cast<uptr_t>(docLine));
    s.execute(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(std::max<sptr_t>(0, disp - s.execute(SCI_LINESONSCREEN) / 2)));
    sync();
}

} // namespace npp::ui
