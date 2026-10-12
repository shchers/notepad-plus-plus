#include "ui/FindBar.h"

#include "core/SearchEngine.h"

#include "FindReplaceDlg_rc.h"

namespace npp::ui {

FindBar::FindBar(std::function<EditorView*()> activeView, const core::Translator& tr)
    : activeView_(std::move(activeView)), tr_(tr)
{
    bar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar_, 4);
    gtk_widget_set_margin_end(bar_, 4);
    gtk_widget_set_margin_top(bar_, 2);
    gtk_widget_set_margin_bottom(bar_, 2);
    gtk_widget_set_no_show_all(bar_, TRUE);

    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(+[](FindBar* b) { b->hide(); }), this);
    label_ = gtk_label_new_with_mnemonic("");
    entry_ = gtk_search_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(entry_), 30);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label_), entry_);
    GtkWidget* prev = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget* next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
    matchCase_ = gtk_check_button_new_with_mnemonic("");
    highlightAll_ = gtk_check_button_new_with_mnemonic("");
    statusLabel_ = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(statusLabel_), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(statusLabel_), 0);
    for (GtkWidget* w : {close, label_, entry_, prev, next, matchCase_, highlightAll_})
        gtk_box_pack_start(GTK_BOX(bar_), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar_), statusLabel_, TRUE, TRUE, 0);

    // As upstream: typing searches from the start of the current match.
    g_signal_connect_swapped(entry_, "search-changed", G_CALLBACK(+[](FindBar* b) { b->search(true, true); }), this);
    g_signal_connect_swapped(entry_, "activate", G_CALLBACK(+[](FindBar* b) { b->search(false, true); }), this);
    g_signal_connect_swapped(entry_, "next-match", G_CALLBACK(+[](FindBar* b) { b->search(false, true); }), this);
    g_signal_connect_swapped(entry_, "previous-match", G_CALLBACK(+[](FindBar* b) { b->search(false, false); }), this);
    g_signal_connect_swapped(entry_, "stop-search", G_CALLBACK(+[](FindBar* b) { b->hide(); }), this);
    g_signal_connect(entry_, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer p) -> gboolean {
                         auto* b = static_cast<FindBar*>(p);
                         if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) && (ev->state & GDK_SHIFT_MASK)) {
                             b->search(false, false);
                             return TRUE;
                         }
                         return FALSE;
                     }), this);
    g_signal_connect_swapped(prev, "clicked", G_CALLBACK(+[](FindBar* b) { b->search(false, false); }), this);
    g_signal_connect_swapped(next, "clicked", G_CALLBACK(+[](FindBar* b) { b->search(false, true); }), this);
    g_signal_connect_swapped(matchCase_, "toggled", G_CALLBACK(+[](FindBar* b) { b->search(true, true); }), this);
    g_signal_connect_swapped(highlightAll_, "toggled", G_CALLBACK(+[](FindBar* b) { b->highlight(); }), this);

    for (GtkWidget* w : {close, label_, entry_, prev, next, matchCase_, highlightAll_, statusLabel_})
        gtk_widget_show(w);
    retranslate();
}

void FindBar::retranslate()
{
    gtk_label_set_text_with_mnemonic(GTK_LABEL(label_), core::Translator::toMnemonic(tr_.dialog("IncrementalFind", IDC_INCSTATIC, "Find:")).c_str());
    gtk_button_set_label(GTK_BUTTON(matchCase_), core::Translator::toMnemonic(tr_.dialog("IncrementalFind", IDC_INCFINDMATCHCASE, "Match &case")).c_str());
    gtk_button_set_label(GTK_BUTTON(highlightAll_), core::Translator::toMnemonic(tr_.dialog("IncrementalFind", IDC_INCFINDHILITEALL, "&Highlight all")).c_str());
}

void FindBar::show()
{
    if (EditorView* v = activeView_()) {
        const std::string sel = v->selectedText();
        if (!sel.empty() && sel.find('\n') == std::string::npos)
            gtk_entry_set_text(GTK_ENTRY(entry_), sel.c_str());
    }
    gtk_widget_show(bar_);
    gtk_widget_grab_focus(entry_);
    gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
}

void FindBar::hide()
{
    gtk_widget_hide(bar_);
    if (EditorView* v = activeView_()) {
        core::SearchEngine(v->sci()).clearMarks(core::kIncrementalIndicator);
        gtk_widget_grab_focus(v->widget());
    }
}

bool FindBar::isVisible() const
{
    return gtk_widget_get_visible(bar_);
}

void FindBar::setText(const std::string& s)
{
    gtk_entry_set_text(GTK_ENTRY(entry_), s.c_str()); // searches through "search-changed"
    search(true, true);
}

void FindBar::setMatchCase(bool on)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(matchCase_), on);
}

void FindBar::setHighlightAll(bool on)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(highlightAll_), on);
}

void FindBar::setStatus(const std::string& s, bool error)
{
    status_ = s;
    gtk_label_set_text(GTK_LABEL(statusLabel_), s.c_str());
    GtkStyleContext* ctx = gtk_widget_get_style_context(entry_);
    if (error)
        gtk_style_context_add_class(ctx, "error");
    else
        gtk_style_context_remove_class(ctx, "error");
}

void FindBar::search(bool fromSelectionStart, bool forward)
{
    EditorView* v = activeView_();
    if (!v)
        return;
    core::FindOptions o;
    o.what = gtk_entry_get_text(GTK_ENTRY(entry_));
    o.matchCase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(matchCase_));
    o.forward = forward;
    o.wrap = true;
    if (o.what.empty()) {
        setStatus("", false);
        highlight();
        return;
    }
    if (fromSelectionStart) { // keep the current match while it still matches
        const sptr_t a = v->execute(SCI_GETSELECTIONSTART);
        v->execute(SCI_SETSEL, static_cast<uptr_t>(a), a);
    }
    switch (core::SearchEngine(v->sci()).findNext(o)) {
    case core::FindStatus::Found: setStatus("", false); break;
    case core::FindStatus::Wrapped:
        setStatus(forward ? tr_.misc("IncrementalFind-FSEndReached", "Reached end of page, continued from top")
                          : tr_.misc("IncrementalFind-FSTopReached", "Reached top of page, continued from bottom"),
                  false);
        break;
    default:
        setStatus(tr_.misc("IncrementalFind-FSNotFound", "Phrase not found"), true);
        break;
    }
    highlight();
}

void FindBar::highlight()
{
    EditorView* v = activeView_();
    if (!v)
        return;
    core::SearchEngine e(v->sci());
    e.clearMarks(core::kIncrementalIndicator);
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(highlightAll_)))
        return;
    core::FindOptions o;
    o.what = gtk_entry_get_text(GTK_ENTRY(entry_));
    o.matchCase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(matchCase_));
    o.wrap = true;
    if (o.what.empty())
        return;
    const int n = e.markAll(o, core::kIncrementalIndicator);
    if (status_.empty() || n > 0) // upstream shows the count next to the field
        setStatus(n == 1 ? "1 match" : std::to_string(n) + " matches", n == 0);
}

} // namespace npp::ui
