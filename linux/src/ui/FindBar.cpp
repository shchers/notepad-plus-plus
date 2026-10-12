#include "ui/FindBar.h"

#include <BoostRegexSearch.h> // Notepad++ SCFIND_REGEXP_* extension flags

namespace npp::ui {

int SearchOptions::scintillaFlags(Op op) const
{
    int f = 0;
    if (matchCase)
        f |= SCFIND_MATCHCASE;
    if (wholeWord)
        f |= SCFIND_WHOLEWORD;
    if (regex) {
        f |= SCFIND_REGEXP | SCFIND_REGEXP_SKIPCRLFASONE; // routed to BoostRegexSearch (SCI_OWNREGEX)
        switch (op) {
        case Op::FindNext: f |= SCFIND_REGEXP_EMPTYMATCH_ALL; break;
        case Op::VerifyForReplace: f |= SCFIND_REGEXP_EMPTYMATCH_ALL | SCFIND_REGEXP_EMPTYMATCH_ALLOWATSTART; break;
        case Op::ReplaceAll: f |= SCFIND_REGEXP_EMPTYMATCH_NOTAFTERMATCH; break;
        }
    }
    return f;
}

namespace {

// Search [from, to) (backwards when from > to). Returns match start or -1/-2.
sptr_t searchRange(EditorView& v, const SearchOptions& o, sptr_t from, sptr_t to,
                   SearchOptions::Op op = SearchOptions::Op::FindNext)
{
    v.execute(SCI_SETSEARCHFLAGS, static_cast<uptr_t>(o.scintillaFlags(op)));
    v.execute(SCI_SETTARGETRANGE, static_cast<uptr_t>(from), to);
    return v.execute(SCI_SEARCHINTARGET, o.pattern.size(), reinterpret_cast<sptr_t>(o.pattern.c_str()));
}

void selectTarget(EditorView& v)
{
    const sptr_t a = v.execute(SCI_GETTARGETSTART), b = v.execute(SCI_GETTARGETEND);
    v.execute(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(v.lineFromPos(a)));
    v.execute(SCI_SETSEL, static_cast<uptr_t>(a), b);
    v.execute(SCI_SCROLLRANGE, static_cast<uptr_t>(b), a);
}

} // namespace

SearchResult findNext(EditorView& v, const SearchOptions& o, bool forward)
{
    if (o.pattern.empty())
        return SearchResult::NotFound;
    const sptr_t len = v.length();
    const sptr_t selStart = v.execute(SCI_GETSELECTIONSTART), selEnd = v.execute(SCI_GETSELECTIONEND);

    sptr_t pos = forward ? searchRange(v, o, selEnd, len) : searchRange(v, o, selStart, 0);
    if (pos == -2)
        return SearchResult::InvalidRegex;
    if (pos >= 0) {
        selectTarget(v);
        return SearchResult::Found;
    }
    if (!o.wrap)
        return SearchResult::NotFound;
    pos = forward ? searchRange(v, o, 0, len) : searchRange(v, o, len, 0);
    if (pos < 0)
        return pos == -2 ? SearchResult::InvalidRegex : SearchResult::NotFound;
    selectTarget(v);
    return SearchResult::Wrapped;
}

SearchResult replaceNext(EditorView& v, const SearchOptions& o, const std::string& replacement)
{
    if (o.pattern.empty())
        return SearchResult::NotFound;
    const sptr_t a = v.execute(SCI_GETSELECTIONSTART), b = v.execute(SCI_GETSELECTIONEND);
    // Is the current selection exactly a match? Then replace it first.
    if (b > a) {
        const sptr_t m = searchRange(v, o, a, b, SearchOptions::Op::VerifyForReplace);
        if (m == -2)
            return SearchResult::InvalidRegex;
        if (m == a && v.execute(SCI_GETTARGETEND) == b) {
            const unsigned msg = o.regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
            v.execute(msg, replacement.size(), reinterpret_cast<sptr_t>(replacement.c_str()));
            const sptr_t end = v.execute(SCI_GETTARGETEND);
            v.execute(SCI_SETSEL, static_cast<uptr_t>(end), end);
        }
    }
    return findNext(v, o, true);
}

int replaceAll(EditorView& v, const SearchOptions& o, const std::string& replacement)
{
    if (o.pattern.empty())
        return 0;
    const unsigned msg = o.regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
    int count = 0;
    sptr_t pos = 0;
    v.execute(SCI_BEGINUNDOACTION);
    for (;;) {
        const sptr_t len = v.length();
        if (pos > len)
            break;
        const sptr_t m = searchRange(v, o, pos, len, SearchOptions::Op::ReplaceAll);
        if (m == -2) {
            v.execute(SCI_ENDUNDOACTION);
            return count ? count : -1;
        }
        if (m < 0)
            break;
        const bool emptyMatch = v.execute(SCI_GETTARGETEND) == m;
        v.execute(msg, replacement.size(), reinterpret_cast<sptr_t>(replacement.c_str()));
        const sptr_t newEnd = v.execute(SCI_GETTARGETEND);
        ++count;
        // The Boost engine refuses an empty match right after the previous
        // match (EMPTYMATCH_NOTAFTERMATCH); the plain engine needs a manual step.
        pos = newEnd;
        if (emptyMatch && !o.regex) {
            if (newEnd >= v.length())
                break;
            pos = v.execute(SCI_POSITIONAFTER, static_cast<uptr_t>(newEnd));
        }
        if (count > 10'000'000)
            break; // defensive: never spin forever on a pathological pattern
    }
    v.execute(SCI_ENDUNDOACTION);
    return count;
}

// ---------------------------------------------------------------------------

namespace {

GtkWidget* iconButton(const char* icon, const char* tooltip)
{
    GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    return b;
}

} // namespace

FindBar::FindBar(std::function<EditorView*()> activeView) : activeView_(std::move(activeView))
{
    // Plain show/hide rather than a GtkRevealer: showing a child of a mapped
    // container realizes it synchronously, so focusing the entry and the
    // key-release that follows Ctrl+F/Ctrl+H always reach a realized widget.
    bar_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar_), 4);
    GtkWidget* outer = bar_;

    // Row 1: find
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    findEntry_ = gtk_search_entry_new();
    gtk_widget_set_hexpand(findEntry_, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(findEntry_), "Find what");
    GtkWidget* prev = iconButton("go-up-symbolic", "Find previous (Shift+F3)");
    GtkWidget* next = iconButton("go-down-symbolic", "Find next (F3)");
    matchCase_ = gtk_check_button_new_with_mnemonic("Match _case");
    wholeWord_ = gtk_check_button_new_with_mnemonic("_Whole word");
    regex_ = gtk_check_button_new_with_mnemonic("_Regular expression");
    status_ = gtk_label_new(nullptr);
    gtk_widget_set_size_request(status_, 160, -1);
    gtk_label_set_xalign(GTK_LABEL(status_), 0.0f);
    GtkWidget* close = iconButton("window-close-symbolic", "Close (Esc)");
    for (GtkWidget* w : {findEntry_, prev, next, matchCase_, wholeWord_, regex_, status_})
        gtk_box_pack_start(GTK_BOX(row), w, FALSE, FALSE, 0);
    gtk_box_set_child_packing(GTK_BOX(row), findEntry_, TRUE, TRUE, 0, GTK_PACK_START);
    gtk_box_pack_end(GTK_BOX(row), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    // Row 2: replace
    replaceRow_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    replaceEntry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(replaceEntry_), "Replace with");
    gtk_widget_set_hexpand(replaceEntry_, TRUE);
    GtkWidget* replaceBtn = gtk_button_new_with_mnemonic("Re_place");
    GtkWidget* replaceAllBtn = gtk_button_new_with_mnemonic("Replace _All");
    gtk_box_pack_start(GTK_BOX(replaceRow_), replaceEntry_, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(replaceRow_), replaceBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(replaceRow_), replaceAllBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), replaceRow_, FALSE, FALSE, 0);
    // Realise the row's children now, then hide the row and exclude it from
    // the window's show_all(); show(true) only has to toggle the row itself.
    gtk_widget_show_all(replaceRow_);
    gtk_widget_hide(replaceRow_);
    gtk_widget_set_no_show_all(replaceRow_, TRUE);
    // Same for the whole bar: hidden until Ctrl+F / Ctrl+H.
    gtk_widget_show_all(bar_);
    gtk_widget_hide(bar_);
    gtk_widget_set_no_show_all(bar_, TRUE);

    auto onNext = +[](GtkWidget*, gpointer self) { static_cast<FindBar*>(self)->doFind(true); };
    auto onPrev = +[](GtkWidget*, gpointer self) { static_cast<FindBar*>(self)->doFind(false); };
    g_signal_connect(next, "clicked", G_CALLBACK(onNext), this);
    g_signal_connect(prev, "clicked", G_CALLBACK(onPrev), this);
    g_signal_connect(findEntry_, "activate", G_CALLBACK(onNext), this);
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer self) { static_cast<FindBar*>(self)->hide(); }), this);

    g_signal_connect(replaceBtn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer p) {
                         auto* self = static_cast<FindBar*>(p);
                         if (EditorView* v = self->activeView_())
                             self->report(replaceNext(*v, self->options(), gtk_entry_get_text(GTK_ENTRY(self->replaceEntry_))));
                     }), this);
    g_signal_connect(replaceEntry_, "activate", G_CALLBACK(+[](GtkWidget*, gpointer p) {
                         auto* self = static_cast<FindBar*>(p);
                         if (EditorView* v = self->activeView_())
                             self->report(replaceNext(*v, self->options(), gtk_entry_get_text(GTK_ENTRY(self->replaceEntry_))));
                     }), this);
    g_signal_connect(replaceAllBtn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer p) {
                         auto* self = static_cast<FindBar*>(p);
                         if (EditorView* v = self->activeView_()) {
                             const int n = replaceAll(*v, self->options(), gtk_entry_get_text(GTK_ENTRY(self->replaceEntry_)));
                             self->report(n < 0 ? SearchResult::InvalidRegex : SearchResult::Found, n);
                         }
                     }), this);

    // Esc closes and returns focus to the editor; Shift+Enter searches backwards.
    auto onKey = +[](GtkWidget* w, GdkEventKey* ev, gpointer p) -> gboolean {
        auto* self = static_cast<FindBar*>(p);
        // Tab / Shift+Tab hop between the two text fields, as in upstream's dialog.
        if (gtk_widget_get_visible(self->replaceRow_)) {
            if (ev->keyval == GDK_KEY_Tab && w == self->findEntry_) {
                gtk_widget_grab_focus(self->replaceEntry_);
                return TRUE;
            }
            if (ev->keyval == GDK_KEY_ISO_Left_Tab && w == self->replaceEntry_) {
                gtk_widget_grab_focus(self->findEntry_);
                return TRUE;
            }
        }
        if (ev->keyval == GDK_KEY_Escape) {
            self->hide();
            return TRUE;
        }
        if ((ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) && (ev->state & GDK_SHIFT_MASK)) {
            self->doFind(false);
            return TRUE;
        }
        return FALSE;
    };
    g_signal_connect(findEntry_, "key-press-event", G_CALLBACK(onKey), this);
    g_signal_connect(replaceEntry_, "key-press-event", G_CALLBACK(onKey), this);
}

SearchOptions FindBar::options() const
{
    SearchOptions o;
    o.pattern = gtk_entry_get_text(GTK_ENTRY(findEntry_));
    o.matchCase = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(matchCase_));
    o.wholeWord = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(wholeWord_));
    o.regex = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(regex_));
    return o;
}

void FindBar::report(SearchResult r, int replaced)
{
    const char* text = "";
    std::string buf;
    switch (r) {
    case SearchResult::Found:
        if (replaced >= 0) {
            buf = std::to_string(replaced) + (replaced == 1 ? " occurrence replaced" : " occurrences replaced");
            text = buf.c_str();
        }
        break;
    case SearchResult::Wrapped: text = "Search wrapped"; break;
    case SearchResult::NotFound: text = "Can't find the text"; break;
    case SearchResult::InvalidRegex: text = "Invalid regular expression"; break;
    }
    gtk_label_set_text(GTK_LABEL(status_), text);
}

void FindBar::doFind(bool forward)
{
    if (EditorView* v = activeView_())
        report(findNext(*v, options(), forward));
}

void FindBar::show(bool withReplace)
{
    gtk_widget_set_visible(replaceRow_, withReplace);
    if (EditorView* v = activeView_()) {
        const std::string sel = v->selectedText();
        if (!sel.empty() && sel.find('\n') == std::string::npos)
            gtk_entry_set_text(GTK_ENTRY(findEntry_), sel.c_str());
    }
    gtk_label_set_text(GTK_LABEL(status_), "");
    gtk_widget_show(bar_);
    gtk_widget_grab_focus(findEntry_);
    gtk_editable_select_region(GTK_EDITABLE(findEntry_), 0, -1);
}

void FindBar::hide()
{
    gtk_widget_hide(bar_);
    if (EditorView* v = activeView_())
        gtk_widget_grab_focus(v->widget());
}

bool FindBar::isVisible() const
{
    return gtk_widget_get_visible(bar_);
}

void FindBar::findAgain(bool forward)
{
    if (gtk_entry_get_text_length(GTK_ENTRY(findEntry_)) == 0) {
        show(false);
        return;
    }
    doFind(forward);
}

} // namespace npp::ui
