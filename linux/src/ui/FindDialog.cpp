#include "ui/FindDialog.h"

#include "FindReplaceDlg_rc.h"

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

// Upstream FindReplaceDlg::saveInMacro operation kinds.
constexpr int kOpFind = 1, kOpReplace = 2, kOpFif = 4, kOpGlobal = 8;
constexpr int kIdOk = 1, kIdCancel = 2; // Win32 IDOK / IDCANCEL
constexpr int kBookmarkMarker = 24;

unsigned tabBit(FindDialog::Tab t) { return 1u << static_cast<int>(t); }

std::string comboText(GtkWidget* combo)
{
    return gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))));
}

void setComboText(GtkWidget* combo, const std::string& s)
{
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), s.c_str());
}

void fillCombo(GtkWidget* combo, const std::vector<std::string>& items)
{
    const std::string keep = comboText(combo);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    for (const std::string& s : items)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), s.c_str());
    setComboText(combo, keep);
}

void pushHistory(std::vector<std::string>& h, const std::string& s)
{
    if (s.empty())
        return;
    h.erase(std::remove(h.begin(), h.end(), s), h.end());
    h.insert(h.begin(), s);
    if (h.size() > platform::FindSettings::kMaxHistory)
        h.resize(platform::FindSettings::kMaxHistory);
}

bool active(GtkWidget* toggle) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)); }

// The selection on one line, else the word at the caret (upstream
// setSearchTextWithSettings with "select word under caret").
std::string selectionOrWord(EditorView& v)
{
    std::string s = v.selectedText();
    if (s.empty()) {
        const sptr_t p = v.currentPos();
        const sptr_t a = v.execute(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(p), true);
        const sptr_t b = v.execute(SCI_WORDENDPOSITION, static_cast<uptr_t>(p), true);
        s = v.sci().range(a, b);
    }
    if (s.find_first_of("\r\n") != std::string::npos || s.size() > 1024)
        return {};
    return s;
}

} // namespace

FindDialog::FindDialog(GtkWindow* parent, const core::Translator& tr, platform::FindSettings& settings, FindHost host)
    : parent_(parent), tr_(tr), st_(settings), host_(std::move(host))
{
    build();
    loadFields();
}

FindDialog::~FindDialog()
{
    if (window_)
        gtk_widget_destroy(window_);
}

std::string FindDialog::label(int id, const char* fallback) const
{
    return core::Translator::toMnemonic(tr_.dialog("Find", id, fallback));
}

void FindDialog::build()
{
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    gtk_window_set_type_hint(GTK_WINDOW(window_), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window_), 640, -1);
    g_signal_connect(window_, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventKey* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<FindDialog*>(p);
                         if (ev->keyval == GDK_KEY_Escape) {
                             self->hide();
                             return TRUE;
                         }
                         if (ev->keyval == GDK_KEY_F3) { // as in the editor
                             self->findNext(!(ev->state & GDK_SHIFT_MASK));
                             return TRUE;
                         }
                         (void)w;
                         return FALSE;
                     }), this);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(window_), outer);

    // Tabs as upstream: a tab strip over one shared set of controls.
    tabs_ = gtk_notebook_new();
    gtk_notebook_set_show_border(GTK_NOTEBOOK(tabs_), FALSE);
    for (int i = 0; i < 4; ++i)
        gtk_notebook_append_page(GTK_NOTEBOOK(tabs_), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), gtk_label_new(""));
    g_signal_connect_after(tabs_, "switch-page", G_CALLBACK(+[](GtkNotebook*, GtkWidget*, guint, gpointer p) {
                               static_cast<FindDialog*>(p)->updateTab();
                           }), this);
    gtk_box_pack_start(GTK_BOX(outer), tabs_, FALSE, FALSE, 0);

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(body), left, TRUE, TRUE, 0);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_box_pack_start(GTK_BOX(left), grid, FALSE, FALSE, 0);
    auto row = [&](int r, GtkWidget*& combo, GtkWidget** cells) {
        GtkWidget* l = gtk_label_new_with_mnemonic("");
        gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
        combo = gtk_combo_box_text_new_with_entry();
        gtk_widget_set_hexpand(combo, TRUE);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), combo);
        gtk_entry_set_activates_default(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), TRUE);
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), combo, 1, r, 1, 1);
        if (cells) {
            cells[0] = l;
            cells[1] = combo;
        }
        return l;
    };
    GtkWidget* whatLabel = row(0, what_, nullptr);
    g_object_set_data(G_OBJECT(window_), "what-label", whatLabel);
    row(1, with_, withRow_);
    row(2, filters_, filtersRow_);
    row(3, dir_, dirRow_);
    GtkWidget* fromDoc = gtk_button_new_with_label("<<");
    GtkWidget* browse = gtk_button_new_with_label("...");
    gtk_grid_attach(GTK_GRID(grid), fromDoc, 2, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), browse, 3, 3, 1, 1);
    dirRow_[2] = fromDoc;
    dirRow_[3] = browse;
    g_signal_connect(fromDoc, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                         auto* self = static_cast<FindDialog*>(p);
                         if (self->host_.currentDirectory) {
                             const fs::path d = self->host_.currentDirectory();
                             if (!d.empty())
                                 self->setDirectory(d.string());
                         }
                     }), this);
    g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) {
                         auto* self = static_cast<FindDialog*>(p);
                         GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
                             self->misc("find-in-files-select-folder", "Select a folder to search from").c_str(),
                             GTK_WINDOW(self->window_), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");
                         const std::string cur = comboText(self->dir_);
                         if (!cur.empty())
                             gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), cur.c_str());
                         if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
                             if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
                                 self->setDirectory(fn);
                                 g_free(fn);
                             }
                         g_object_unref(dlg);
                     }), this);

    // Options
    GtkWidget* opts = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(opts), 18);
    gtk_box_pack_start(GTK_BOX(left), opts, FALSE, FALSE, 0);
    GtkWidget* checks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_grid_attach(GTK_GRID(opts), checks, 0, 0, 1, 1);
    auto check = [&](GtkWidget* box) {
        GtkWidget* c = gtk_check_button_new_with_mnemonic("");
        gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
        return c;
    };
    bookmark_ = check(checks);
    purge_ = check(checks);
    backward_ = check(checks);
    wholeWord_ = check(checks);
    matchCase_ = check(checks);
    wrap_ = check(checks);
    recursive_ = check(checks);
    hidden_ = check(checks);

    modeFrame_ = gtk_frame_new("");
    GtkWidget* modes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(modes), 6);
    gtk_container_add(GTK_CONTAINER(modeFrame_), modes);
    normal_ = gtk_radio_button_new_with_mnemonic(nullptr, "");
    extended_ = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(normal_), "");
    GtkWidget* reRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    regex_ = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(normal_), "");
    dotNl_ = gtk_check_button_new_with_mnemonic("");
    gtk_box_pack_start(GTK_BOX(reRow), regex_, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(reRow), dotNl_, FALSE, FALSE, 0);
    for (GtkWidget* w : {normal_, extended_})
        gtk_box_pack_start(GTK_BOX(modes), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(modes), reRow, FALSE, FALSE, 0);
    g_signal_connect_swapped(regex_, "toggled", G_CALLBACK(+[](FindDialog* self) { self->updateTab(); }), this);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(right), modeFrame_, FALSE, FALSE, 0);
    inSelection_ = check(right);
    gtk_grid_attach(GTK_GRID(opts), right, 1, 0, 1, 1);

    // Buttons, per tab (upstream layout).
    GtkWidget* btnBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(body), btnBox, FALSE, FALSE, 0);
    const unsigned F = tabBit(Tab::Find), R = tabBit(Tab::Replace), I = tabBit(Tab::FindInFiles), M = tabBit(Tab::Mark);
    const std::pair<int, unsigned> btns[] = {
        {kIdOk, F | R}, {IDCCOUNTALL, F}, {IDC_FINDALL_OPENEDFILES, F}, {IDC_FINDALL_CURRENTFILE, F},
        {IDREPLACE, R}, {IDREPLACEALL, R}, {IDC_REPLACE_OPENEDFILES, R},
        {IDD_FINDINFILES_FIND_BUTTON, I}, {IDD_FINDINFILES_REPLACEINFILES, I},
        {IDCMARKALL, M}, {IDC_CLEAR_ALL, M}, {IDC_COPY_MARKED_TEXT, M}, {kIdCancel, F | R | I | M},
    };
    for (const auto& [id, tabs] : btns) {
        GtkWidget* b = gtk_button_new_with_mnemonic("");
        g_object_set_data(G_OBJECT(b), "npp-id", GINT_TO_POINTER(id));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer p) {
                             static_cast<FindDialog*>(p)->click(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "npp-id")));
                         }), this);
        gtk_box_pack_start(GTK_BOX(btnBox), b, FALSE, FALSE, 0);
        buttons_.push_back({id, b, tabs});
    }

    statusLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(statusLabel_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(statusLabel_), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(statusLabel_), TRUE);
    gtk_box_pack_end(GTK_BOX(outer), statusLabel_, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    gtk_widget_show_all(outer);
    retranslate();
}

void FindDialog::retranslate()
{
    const char* titles[][2] = {{"titleFind", "Find"}, {"titleReplace", "Replace"}, {"titleFindInFiles", "Find in Files"}, {"titleMark", "Mark"}};
    for (int i = 0; i < 4; ++i)
        gtk_notebook_set_tab_label_text(GTK_NOTEBOOK(tabs_), gtk_notebook_get_nth_page(GTK_NOTEBOOK(tabs_), i),
                                        tr_.dialogAttr("Find", titles[i][0], titles[i][1]).c_str());
    auto setLabel = [&](GtkWidget* w, int id, const char* fallback) {
        if (GTK_IS_LABEL(w))
            gtk_label_set_text_with_mnemonic(GTK_LABEL(w), label(id, fallback).c_str());
        else
            gtk_button_set_label(GTK_BUTTON(w), label(id, fallback).c_str());
    };
    setLabel(GTK_WIDGET(g_object_get_data(G_OBJECT(window_), "what-label")), IDFINDWHAT_STATIC, "&Find what:");
    setLabel(withRow_[0], ID_STATICTEXT_REPLACE, "Rep&lace with:");
    setLabel(filtersRow_[0], IDD_FINDINFILES_FILTERS_STATIC, "Filter&s:");
    setLabel(dirRow_[0], IDD_FINDINFILES_DIR_STATIC, "Dir&ectory:");
    gtk_widget_set_tooltip_text(dirRow_[2], misc("find-in-files-dir-from-active-doc-tip", "Fill Directory field based on active document").c_str());
    gtk_widget_set_tooltip_text(filtersRow_[1], misc("find-in-files-filter-tip", "").c_str());
    setLabel(bookmark_, IDC_MARKLINE_CHECK, "Book&mark line");
    setLabel(purge_, IDC_PURGE_CHECK, "Purge for each search");
    setLabel(backward_, IDC_BACKWARDDIRECTION, "Backward direction");
    setLabel(wholeWord_, IDWHOLEWORD, "Match &whole word only");
    setLabel(matchCase_, IDMATCHCASE, "Match &case");
    setLabel(wrap_, IDWRAP, "Wra&p around");
    setLabel(recursive_, IDD_FINDINFILES_RECURSIVE_CHECK, "In all su&b-folders");
    setLabel(hidden_, IDD_FINDINFILES_INHIDDENDIR_CHECK, "In &hidden folders");
    setLabel(inSelection_, IDC_IN_SELECTION_CHECK, "In select&ion");
    gtk_frame_set_label(GTK_FRAME(modeFrame_), core::Translator::plain(tr_.dialog("Find", IDC_MODE_STATIC, "Search mode")).c_str());
    setLabel(normal_, IDNORMAL, "&Normal");
    setLabel(extended_, IDEXTENDED, "E&xtended (\\n, \\r, \\t, \\0, \\x...)");
    setLabel(regex_, IDREGEXP, "Re&gular expression");
    setLabel(dotNl_, IDREDOTMATCHNL, "&. matches newline");
    for (const Button& b : buttons_) {
        const char* fb = "";
        switch (b.id) {
        case kIdOk: fb = "Find Next"; break;
        case IDCCOUNTALL: fb = "Coun&t"; break;
        case IDC_FINDALL_OPENEDFILES: fb = "Find All in All &Opened Documents"; break;
        case IDC_FINDALL_CURRENTFILE: fb = "Find All in Current Document"; break;
        case IDREPLACE: fb = "&Replace"; break;
        case IDREPLACEALL: fb = "Replace &All"; break;
        case IDC_REPLACE_OPENEDFILES: fb = "Replace All in All &Opened Documents"; break;
        case IDD_FINDINFILES_FIND_BUTTON: fb = "Find All"; break;
        case IDD_FINDINFILES_REPLACEINFILES: fb = "Replace in Files"; break;
        case IDCMARKALL: fb = "Mark All"; break;
        case IDC_CLEAR_ALL: fb = "Clear all marks"; break;
        case IDC_COPY_MARKED_TEXT: fb = "Copy Marked Text"; break;
        case kIdCancel: fb = "Close"; break;
        }
        gtk_button_set_label(GTK_BUTTON(b.widget), label(b.id, fb).c_str());
        gtk_button_set_use_underline(GTK_BUTTON(b.widget), TRUE);
    }
    updateTab();
}

void FindDialog::updateTab()
{
    const Tab tab = static_cast<Tab>(std::max(0, gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs_))));
    const bool fif = tab == Tab::FindInFiles, mark = tab == Tab::Mark;
    for (GtkWidget* w : withRow_)
        gtk_widget_set_visible(w, tab == Tab::Replace || fif);
    for (GtkWidget* w : filtersRow_)
        gtk_widget_set_visible(w, fif);
    for (GtkWidget* w : dirRow_)
        gtk_widget_set_visible(w, fif);
    gtk_widget_set_visible(recursive_, fif);
    gtk_widget_set_visible(hidden_, fif);
    gtk_widget_set_visible(bookmark_, mark);
    gtk_widget_set_visible(purge_, mark);
    gtk_widget_set_visible(backward_, !fif && !mark);
    gtk_widget_set_visible(wrap_, !fif);
    gtk_widget_set_visible(inSelection_, !fif);
    gtk_widget_set_sensitive(dotNl_, active(regex_));
    GtkWidget* deflt = nullptr;
    for (const Button& b : buttons_) {
        const bool on = (b.tabs & tabBit(tab)) != 0;
        gtk_widget_set_visible(b.widget, on);
        if (on && !deflt && b.id != kIdCancel)
            deflt = b.widget;
    }
    if (deflt) {
        gtk_widget_set_can_default(deflt, TRUE);
        gtk_window_set_default(GTK_WINDOW(window_), deflt);
    }
    const char* titles[][2] = {{"titleFind", "Find"}, {"titleReplace", "Replace"}, {"titleFindInFiles", "Find in Files"}, {"titleMark", "Mark"}};
    gtk_window_set_title(GTK_WINDOW(window_), tr_.dialogAttr("Find", titles[static_cast<int>(tab)][0], titles[static_cast<int>(tab)][1]).c_str());
}

void FindDialog::loadFields()
{
    fillCombo(what_, st_.what);
    fillCombo(with_, st_.with);
    fillCombo(filters_, st_.filters);
    fillCombo(dir_, st_.directories);
    setComboText(what_, st_.what.empty() ? "" : st_.what.front());
    setComboText(with_, st_.with.empty() ? "" : st_.with.front());
    setComboText(filters_, st_.filters.empty() ? "*.*" : st_.filters.front());
    setComboText(dir_, st_.directories.empty() ? "" : st_.directories.front());
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(matchCase_), st_.matchCase);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wholeWord_), st_.wholeWord);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wrap_), st_.wrap);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(backward_), st_.backward);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dotNl_), st_.dotMatchesNewline);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inSelection_), st_.inSelection);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(recursive_), st_.recursive);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hidden_), st_.hidden);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bookmark_), st_.bookmarkLine);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(purge_), st_.purge);
    GtkWidget* mode = st_.mode == 2 ? regex_ : st_.mode == 1 ? extended_ : normal_;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mode), TRUE);
}

void FindDialog::remember()
{
    pushHistory(st_.what, comboText(what_));
    const Tab tab = static_cast<Tab>(gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs_)));
    if (tab == Tab::Replace || tab == Tab::FindInFiles)
        pushHistory(st_.with, comboText(with_));
    if (tab == Tab::FindInFiles) {
        pushHistory(st_.filters, comboText(filters_));
        pushHistory(st_.directories, comboText(dir_));
    }
    const core::FindOptions o = options();
    st_.mode = static_cast<int>(o.mode);
    st_.matchCase = o.matchCase;
    st_.wholeWord = o.wholeWord;
    st_.wrap = o.wrap;
    st_.backward = !o.forward;
    st_.dotMatchesNewline = o.dotMatchesNewline;
    st_.inSelection = o.inSelection;
    st_.recursive = o.recursive;
    st_.hidden = o.hidden;
    st_.bookmarkLine = o.bookmarkLine;
    st_.purge = o.purge;
    fillCombo(what_, st_.what);
    fillCombo(with_, st_.with);
    fillCombo(filters_, st_.filters);
    fillCombo(dir_, st_.directories);
}

core::FindOptions FindDialog::options() const
{
    core::FindOptions o;
    o.what = comboText(what_);
    o.with = comboText(with_);
    o.mode = active(regex_) ? core::SearchMode::Regex : active(extended_) ? core::SearchMode::Extended : core::SearchMode::Normal;
    o.matchCase = active(matchCase_);
    o.wholeWord = active(wholeWord_);
    o.dotMatchesNewline = active(dotNl_);
    o.wrap = active(wrap_);
    o.forward = !active(backward_);
    o.inSelection = active(inSelection_);
    o.bookmarkLine = active(bookmark_);
    o.purge = active(purge_);
    o.directory = comboText(dir_);
    o.filters = comboText(filters_);
    o.recursive = active(recursive_);
    o.hidden = active(hidden_);
    return o;
}

void FindDialog::setSearchText(const std::string& s)
{
    setComboText(what_, s);
}

void FindDialog::setDirectory(const std::string& dir)
{
    setComboText(dir_, dir);
}

void FindDialog::show(Tab tab)
{
    if (EditorView* v = view()) {
        const std::string s = selectionOrWord(*v);
        if (!s.empty())
            setSearchText(s);
        // A selection over several lines: search inside it, as upstream.
        const sptr_t a = v->execute(SCI_GETSELECTIONSTART), b = v->execute(SCI_GETSELECTIONEND);
        if (v->lineFromPos(a) != v->lineFromPos(b))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inSelection_), TRUE);
        else if (a == b)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inSelection_), FALSE);
    }
    if (tab == Tab::FindInFiles && comboText(dir_).empty() && host_.currentDirectory) {
        const fs::path d = host_.currentDirectory();
        if (!d.empty())
            setDirectory(d.string());
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs_), static_cast<int>(tab));
    updateTab();
    gtk_window_present(GTK_WINDOW(window_));
    GtkWidget* entry = gtk_bin_get_child(GTK_BIN(what_));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

void FindDialog::hide()
{
    gtk_widget_hide(window_);
    if (EditorView* v = view())
        gtk_widget_grab_focus(v->widget());
}

bool FindDialog::isVisible() const
{
    return gtk_widget_get_visible(window_);
}

void FindDialog::setStatus(const std::string& msg, bool error)
{
    status_ = msg;
    if (error) {
        gchar* esc = g_markup_escape_text(msg.c_str(), -1);
        const std::string markup = std::string("<span foreground=\"#C00000\">") + esc + "</span>";
        g_free(esc);
        gtk_label_set_markup(GTK_LABEL(statusLabel_), markup.c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(statusLabel_), msg.c_str());
    }
}

std::string FindDialog::scopeInfo(const core::FindOptions& o) const
{
    if (o.inSelection)
        return misc("find-status-scope-selection", "in selected text");
    if (o.wrap)
        return misc("find-status-scope-all", "in entire file");
    return o.forward ? misc("find-status-scope-forward", "from caret to end-of-file")
                     : misc("find-status-scope-backward", "from start-of-file to caret");
}

void FindDialog::record(int cmd, const core::FindOptions& o, int kind)
{
    // Upstream FindReplaceDlg::saveInMacro, step by step.
    if (!host_.record)
        return;
    using core::MacroStep;
    host_.record(MacroStep::search(IDC_FRCOMMAND_INIT, 0));
    host_.record(MacroStep::search(IDFINDWHAT, cmd == IDC_CLEAR_ALL ? std::string() : o.what));
    int b = (o.wholeWord ? IDF_WHOLEWORD : 0) | (o.matchCase ? IDF_MATCHCASE : 0) | (o.dotMatchesNewline ? IDF_REDOTMATCHNL : 0);
    host_.record(MacroStep::search(IDNORMAL, static_cast<int>(o.mode)));
    if (cmd == IDCMARKALL)
        b |= (o.purge ? IDF_PURGE_CHECK : 0) | (o.bookmarkLine ? IDF_MARKLINE_CHECK : 0);
    if (kind & kOpReplace)
        host_.record(MacroStep::search(IDREPLACEWITH, o.with));
    if (kind & kOpFif) {
        host_.record(MacroStep::search(IDD_FINDINFILES_DIR_COMBO, o.directory));
        host_.record(MacroStep::search(IDD_FINDINFILES_FILTERS_COMBO, o.filters));
        b |= (o.recursive ? IDF_FINDINFILES_RECURSIVE_CHECK : 0) | (o.hidden ? IDF_FINDINFILES_INHIDDENDIR_CHECK : 0);
    } else if (!(kind & kOpGlobal)) {
        b |= (o.inSelection ? IDF_IN_SELECTION_CHECK : 0) | (o.wrap ? IDF_WRAP : 0) | (o.forward ? IDF_WHICH_DIRECTION : 0);
    } else if (kind == kOpFind + kOpGlobal) {
        b |= o.inSelection ? IDF_IN_SELECTION_CHECK : 0;
    }
    if (cmd == IDC_CLEAR_ALL)
        b = (o.bookmarkLine ? IDF_MARKLINE_CHECK : 0) | (o.inSelection ? IDF_IN_SELECTION_CHECK : 0);
    host_.record(MacroStep::search(IDC_FRCOMMAND_BOOLEANS, b));
    host_.record(MacroStep::search(IDC_FRCOMMAND_EXEC, cmd));
}

void FindDialog::click(int id)
{
    if (id == kIdCancel) {
        hide();
        return;
    }
    remember();
    const core::FindOptions o = options();
    switch (id) {
    case kIdOk:
        record(kIdOk, o, kOpFind);
        doFindNext(o);
        break;
    case IDCCOUNTALL:
        record(IDCCOUNTALL, o, kOpFind);
        doCount(o);
        break;
    case IDC_FINDALL_OPENEDFILES:
        record(IDC_FINDALL_OPENEDFILES, o, kOpFind | kOpGlobal);
        doFindAllInOpened(o);
        break;
    case IDC_FINDALL_CURRENTFILE:
        record(IDC_FINDALL_CURRENTFILE, o, kOpFind | kOpGlobal);
        doFindAllInCurrent(o);
        break;
    case IDREPLACE:
        record(IDREPLACE, o, kOpReplace);
        doReplace(o);
        break;
    case IDREPLACEALL:
        record(IDREPLACEALL, o, kOpReplace);
        doReplaceAll(o);
        break;
    case IDC_REPLACE_OPENEDFILES:
        if (host_.confirm
            && !host_.confirm(misc("replace-in-open-docs-confirm-title", "Are you sure?"),
                              misc("replace-in-open-docs-confirm-message", "Are you sure you want to replace all occurrences in all open documents?")))
            break;
        record(IDC_REPLACE_OPENEDFILES, o, kOpReplace | kOpGlobal);
        doReplaceAllInOpened(o);
        break;
    case IDD_FINDINFILES_FIND_BUTTON:
        record(IDD_FINDINFILES_FIND_BUTTON, o, kOpFind | kOpFif);
        doFindInFiles(o);
        break;
    case IDD_FINDINFILES_REPLACEINFILES:
        record(IDD_FINDINFILES_REPLACEINFILES, o, kOpReplace | kOpFif);
        doReplaceInFiles(o);
        break;
    case IDCMARKALL:
        record(IDCMARKALL, o, kOpFind);
        doMarkAll(o);
        break;
    case IDC_CLEAR_ALL:
        record(IDC_CLEAR_ALL, o, kOpFind);
        doClearMarks(o);
        break;
    case IDC_COPY_MARKED_TEXT:
        copyMarkedText();
        break;
    default:
        break;
    }
}

// ============================================================================
// Operations
// ============================================================================

core::FindStatus FindDialog::doFindNext(const core::FindOptions& o)
{
    EditorView* v = view();
    if (!v)
        return core::FindStatus::NotFound;
    const core::FindStatus r = core::SearchEngine(v->sci()).findNext(o);
    switch (r) {
    case core::FindStatus::Found: setStatus(""); break;
    case core::FindStatus::Wrapped:
        setStatus(o.forward ? misc("find-status-end-reached", "Find: Reached document end, first occurrence from the top found.")
                            : misc("find-status-top-reached", "Find: Reached document beginning, first occurrence from the bottom found."));
        break;
    case core::FindStatus::NotFound:
        setStatus(core::Translator::format(misc("find-status-cannot-find", "Find: Can't find the text \"$STR_REPLACE$\""), "$STR_REPLACE$", o.what), true);
        gtk_widget_error_bell(window_);
        break;
    case core::FindStatus::InvalidRegex: setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true); break;
    case core::FindStatus::Empty: break;
    }
    return r;
}

core::FindStatus FindDialog::doReplace(const core::FindOptions& o)
{
    EditorView* v = view();
    if (!v)
        return core::FindStatus::NotFound;
    if (v->execute(SCI_GETREADONLY)) {
        setStatus(misc("find-status-replace-readonly", "Replace: Cannot replace text. The current document is read only"), true);
        return core::FindStatus::NotFound;
    }
    bool replaced = false;
    const core::FindStatus r = core::SearchEngine(v->sci()).replace(o, &replaced);
    switch (r) {
    case core::FindStatus::Found:
    case core::FindStatus::Wrapped:
        setStatus(replaced ? misc("find-status-replaced-next-found", "Replace: 1 occurrence was replaced. The next occurrence found.")
                           : (r == core::FindStatus::Wrapped
                                  ? (o.forward ? misc("find-status-replace-end-reached", "Replace: Reached document end, started from top.")
                                               : misc("find-status-replace-top-reached", "Replace: Reached document beginning, started from bottom."))
                                  : std::string()));
        break;
    case core::FindStatus::NotFound:
        setStatus(replaced ? misc("find-status-replaced-next-not-found", "Replace: 1 occurrence was replaced. No more occurrences were found.")
                           : misc("find-status-replace-not-found", "Replace: no occurrence was found"),
                  !replaced);
        break;
    case core::FindStatus::InvalidRegex: setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true); break;
    case core::FindStatus::Empty: break;
    }
    if (host_.afterEdit)
        host_.afterEdit();
    return r;
}

int FindDialog::doReplaceAll(const core::FindOptions& o)
{
    EditorView* v = view();
    if (!v)
        return 0;
    if (v->execute(SCI_GETREADONLY)) {
        setStatus(misc("find-status-replaceall-readonly", "Replace All: Cannot replace text. The current document is read only"), true);
        return 0;
    }
    const int n = core::SearchEngine(v->sci()).replaceAll(o);
    if (n < 0) {
        setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
        return n;
    }
    std::string msg = n == 1 ? misc("find-status-replaceall-1-replaced", "Replace All: 1 occurrence was replaced")
                             : core::Translator::format(misc("find-status-replaceall-nb-replaced", "Replace All: $INT_REPLACE$ occurrences were replaced"),
                                                        "$INT_REPLACE$", std::to_string(n));
    setStatus(msg + " " + scopeInfo(o));
    if (host_.afterEdit)
        host_.afterEdit();
    return n;
}

int FindDialog::doReplaceAllInOpened(const core::FindOptions& in)
{
    core::FindOptions o = in;
    o.inSelection = false;
    o.wrap = true;
    int total = 0;
    if (host_.documents)
        for (const auto& [name, v] : host_.documents()) {
            if (v->execute(SCI_GETREADONLY))
                continue;
            const int n = core::SearchEngine(v->sci()).replaceAll(o);
            if (n < 0) {
                setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
                return n;
            }
            total += n;
        }
    setStatus(total == 1 ? misc("find-status-replaceinopenedfiles-1-replaced", "Replace in Opened Files: 1 occurrence was replaced")
                         : core::Translator::format(misc("find-status-replaceinopenedfiles-nb-replaced",
                                                         "Replace in Opened Files: $INT_REPLACE$ occurrences were replaced"),
                                                    "$INT_REPLACE$", std::to_string(total)));
    if (host_.afterEdit)
        host_.afterEdit();
    return total;
}

int FindDialog::doCount(const core::FindOptions& o)
{
    EditorView* v = view();
    if (!v)
        return 0;
    const int n = core::SearchEngine(v->sci()).countAll(o);
    if (n < 0) {
        setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
        return n;
    }
    const std::string msg = n == 1 ? misc("find-status-count-1-match", "Count: 1 match")
                                   : core::Translator::format(misc("find-status-count-nb-matches", "Count: $INT_REPLACE$ matches"),
                                                              "$INT_REPLACE$", std::to_string(n));
    setStatus(msg + " " + scopeInfo(o), n == 0);
    return n;
}

int FindDialog::doMarkAll(const core::FindOptions& o)
{
    EditorView* v = view();
    if (!v)
        return 0;
    const int n = core::SearchEngine(v->sci()).markAll(o, core::kFoundIndicator, kBookmarkMarker);
    if (n < 0) {
        setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
        return n;
    }
    const std::string msg = n == 1 ? misc("find-status-mark-1-match", "Mark: 1 match")
                                   : core::Translator::format(misc("find-status-mark-nb-matches", "Mark: $INT_REPLACE$ matches"),
                                                              "$INT_REPLACE$", std::to_string(n));
    setStatus(msg + " " + scopeInfo(o), n == 0);
    return n;
}

void FindDialog::doClearMarks(const core::FindOptions& o)
{
    EditorView* v = view();
    if (!v)
        return;
    core::SearchEngine(v->sci()).clearMarks(core::kFoundIndicator);
    if (o.bookmarkLine)
        v->execute(SCI_MARKERDELETEALL, kBookmarkMarker);
    setStatus("");
}

void FindDialog::copyMarkedText()
{
    EditorView* v = view();
    if (!v)
        return;
    const core::Sci s = v->sci();
    std::string out;
    const sptr_t len = s.length();
    for (sptr_t p = 0; p < len;) {
        const sptr_t e = s(SCI_INDICATOREND, core::kFoundIndicator, p);
        if (s(SCI_INDICATORVALUEAT, core::kFoundIndicator, p))
            out += s.range(p, e) + "\n";
        if (e <= p)
            break;
        p = e;
    }
    if (!out.empty())
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), out.c_str(), static_cast<gint>(out.size()));
}

int FindDialog::doFindAllInCurrent(const core::FindOptions& o)
{
    EditorView* v = view();
    SearchResults* res = host_.showResults ? host_.showResults() : nullptr;
    if (!v || !res)
        return 0;
    std::vector<core::Hit> hits;
    const int n = core::SearchEngine(v->sci()).findAll(o, hits);
    if (n < 0) {
        setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
        return n;
    }
    std::string name;
    if (host_.documents)
        for (const auto& [docName, dv] : host_.documents())
            if (dv->execute(SCI_GETDOCPOINTER) == v->execute(SCI_GETDOCPOINTER))
                name = docName;
    res->begin(o.what);
    res->addFile(name, hits);
    res->finish(1, !o.inSelection, o);
    setStatus("");
    return n;
}

int FindDialog::doFindAllInOpened(const core::FindOptions& in)
{
    SearchResults* res = host_.showResults ? host_.showResults() : nullptr;
    if (!res || !host_.documents)
        return 0;
    core::FindOptions o = in;
    o.inSelection = false;
    const auto docs = host_.documents();
    res->begin(o.what);
    int total = 0;
    for (const auto& [name, v] : docs) {
        std::vector<core::Hit> hits;
        const int n = core::SearchEngine(v->sci()).findAll(o, hits, true);
        if (n < 0) {
            setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
            return n;
        }
        res->addFile(name, hits);
        total += n;
    }
    res->finish(static_cast<int>(docs.size()), true, o);
    setStatus("");
    return total;
}

namespace {

// A small progress window with Cancel, pumped while Find in Files runs
// (upstream "Find In Files progress...").
class Progress {
public:
    Progress(GtkWindow* parent, const std::string& title)
    {
        win_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win_), title.c_str());
        gtk_window_set_transient_for(GTK_WINDOW(win_), parent);
        gtk_window_set_modal(GTK_WINDOW(win_), TRUE);
        gtk_window_set_default_size(GTK_WINDOW(win_), 420, -1);
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(box), 12);
        label_ = gtk_label_new("");
        gtk_label_set_ellipsize(GTK_LABEL(label_), PANGO_ELLIPSIZE_MIDDLE);
        bar_ = gtk_progress_bar_new();
        GtkWidget* cancel = gtk_button_new_with_mnemonic("_Cancel");
        g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { *static_cast<bool*>(p) = true; }), &cancelled_);
        g_signal_connect(win_, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
                             *static_cast<bool*>(p) = true;
                             return TRUE;
                         }), &cancelled_);
        gtk_box_pack_start(GTK_BOX(box), label_, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), bar_, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), cancel, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(win_), box);
    }
    ~Progress() { gtk_widget_destroy(win_); }
    // Returns false when cancelled. Shows the window once the work takes a while.
    bool update(const std::string& text, double fraction)
    {
        if (!shown_ && g_get_monotonic_time() - start_ > 300000) {
            gtk_widget_show_all(win_);
            shown_ = true;
        }
        if (shown_ && g_get_monotonic_time() - last_ > 50000) {
            last_ = g_get_monotonic_time();
            gtk_label_set_text(GTK_LABEL(label_), text.c_str());
            if (fraction < 0)
                gtk_progress_bar_pulse(GTK_PROGRESS_BAR(bar_));
            else
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_), fraction);
            while (gtk_events_pending())
                gtk_main_iteration();
        }
        return !cancelled_;
    }

private:
    GtkWidget* win_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* bar_ = nullptr;
    bool cancelled_ = false;
    bool shown_ = false;
    gint64 start_ = g_get_monotonic_time();
    gint64 last_ = 0;
};

} // namespace

int FindDialog::doFindInFiles(const core::FindOptions& o)
{
    SearchResults* res = host_.showResults ? host_.showResults() : nullptr;
    std::error_code ec;
    if (o.what.empty() || o.directory.empty() || !fs::is_directory(o.directory, ec)) {
        setStatus(misc("find-status-search-failed", "Find: Search failed"), true);
        return 0;
    }
    if (core::SearchEngine::pattern(o).empty())
        return 0;
    Progress progress(GTK_WINDOW(window_), misc("find-in-files-progress-title", "Find In Files progress..."));
    const core::FileFilter filter(o.filters.empty() ? "*.*" : o.filters);
    const auto files = core::listFiles(o.directory, filter, o.recursive, o.hidden,
                                       [&](std::size_t n) { return progress.update(std::to_string(n) + " files", -1); });
    EditorView scratch;
    core::FileSearcher searcher(scratch.sci());
    if (res)
        res->begin(o.what);
    int total = 0, searched = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const fs::path& f = files[i].path;
        if (!progress.update(f.string(), static_cast<double>(i) / static_cast<double>(files.size())))
            break;
        core::FileHits fh;
        fh.path = f;
        // An open document is searched as it is in the editor, as upstream.
        if (EditorView* open = host_.viewForFile ? host_.viewForFile(f) : nullptr) {
            core::FindOptions whole = o;
            whole.inSelection = false;
            if (core::SearchEngine(open->sci()).findAll(whole, fh.hits, true) < 0) {
                setStatus(misc("find-status-invalid-re", "Find: Invalid regular expression"), true);
                return -1;
            }
        } else if (!searcher.findInFile(f, o, fh)) {
            continue;
        }
        ++searched;
        total += static_cast<int>(fh.hits.size());
        if (res)
            res->addFile(f.string(), fh.hits);
    }
    if (res)
        res->finish(searched, true, o);
    setStatus("");
    return total;
}

int FindDialog::doReplaceInFiles(const core::FindOptions& o, bool confirm)
{
    std::error_code ec;
    if (o.directory.empty() || !fs::is_directory(o.directory, ec) || core::SearchEngine::pattern(o).empty()) {
        setStatus(misc("find-status-search-failed", "Find: Search failed"), true);
        return 0;
    }
    if (confirm && host_.confirm
        && !host_.confirm(misc("replace-in-files-confirm-title", "Are you sure?"),
                          misc("replace-in-files-confirm-directory", "Are you sure you want to replace all occurrences in:") + "\n" + o.directory +
                              "\n\n" + misc("replace-in-files-confirm-filetype", "For file type:") + "\n" +
                              (o.filters.empty() ? "*.*" : o.filters)))
        return 0;
    Progress progress(GTK_WINDOW(window_), misc("replace-in-files-progress-title", "Replace In Files progress..."));
    const core::FileFilter filter(o.filters.empty() ? "*.*" : o.filters);
    const auto files = core::listFiles(o.directory, filter, o.recursive, o.hidden,
                                       [&](std::size_t n) { return progress.update(std::to_string(n) + " files", -1); });
    EditorView scratch;
    core::FileSearcher searcher(scratch.sci());
    core::FindOptions whole = o;
    whole.inSelection = false;
    whole.wrap = true;
    int total = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const fs::path& f = files[i].path;
        if (!progress.update(f.string(), static_cast<double>(i) / static_cast<double>(files.size())))
            break;
        // Open documents are changed in the editor (and stay unsaved), as upstream.
        if (EditorView* open = host_.viewForFile ? host_.viewForFile(f) : nullptr) {
            if (!open->execute(SCI_GETREADONLY))
                total += std::max(0, core::SearchEngine(open->sci()).replaceAll(whole));
            continue;
        }
        const int n = searcher.replaceInFile(f, whole);
        if (n > 0)
            total += n;
    }
    setStatus(total == 1 ? misc("find-status-replaceinfiles-1-replaced", "Replace in Files: 1 occurrence was replaced")
                         : core::Translator::format(misc("find-status-replaceinfiles-nb-replaced", "Replace in Files: $INT_REPLACE$ occurrences were replaced"),
                                                    "$INT_REPLACE$", std::to_string(total)));
    if (host_.afterEdit)
        host_.afterEdit();
    return total;
}

// ============================================================================
// Menu commands and macros
// ============================================================================

void FindDialog::findNext(bool forward)
{
    core::FindOptions o = options();
    if (o.what.empty()) {
        show(Tab::Find);
        return;
    }
    o.forward = forward;
    o.inSelection = false;
    remember();
    doFindNext(o);
}

void FindDialog::selectAndFind(bool forward, bool volatileSearch)
{
    EditorView* v = view();
    if (!v)
        return;
    const std::string s = selectionOrWord(*v);
    if (s.empty())
        return;
    core::FindOptions o;
    if (volatileSearch) { // upstream: match case and whole word off, normal mode
        o.what = s;
    } else {
        setSearchText(s);
        remember();
        o = options();
        o.what = s;
        o.mode = core::SearchMode::Normal;
    }
    o.forward = forward;
    o.inSelection = false;
    o.wrap = true;
    doFindNext(o);
}

void FindDialog::execSaved(int what, std::int64_t iv, const std::string& s)
{
    // Upstream FindReplaceDlg::execSavedCommand.
    if (what == IDC_FRCOMMAND_INIT) {
        savedEnv_ = std::make_unique<core::FindOptions>();
        return;
    }
    if (!savedEnv_)
        return;
    core::FindOptions& e = *savedEnv_;
    switch (what) {
    case IDFINDWHAT: e.what = s; break;
    case IDREPLACEWITH: e.with = s; break;
    case IDD_FINDINFILES_DIR_COMBO: e.directory = s; break;
    case IDD_FINDINFILES_FILTERS_COMBO: e.filters = s; break;
    case IDNORMAL: e.mode = static_cast<core::SearchMode>(std::clamp<std::int64_t>(iv, 0, 2)); break;
    case IDC_FRCOMMAND_BOOLEANS:
        e.wholeWord = (iv & IDF_WHOLEWORD) != 0;
        e.matchCase = (iv & IDF_MATCHCASE) != 0;
        e.recursive = (iv & IDF_FINDINFILES_RECURSIVE_CHECK) != 0;
        e.hidden = (iv & IDF_FINDINFILES_INHIDDENDIR_CHECK) != 0;
        e.purge = (iv & IDF_PURGE_CHECK) != 0;
        e.bookmarkLine = (iv & IDF_MARKLINE_CHECK) != 0;
        e.inSelection = (iv & IDF_IN_SELECTION_CHECK) != 0;
        e.wrap = (iv & IDF_WRAP) != 0;
        e.forward = (iv & IDF_WHICH_DIRECTION) != 0;
        e.dotMatchesNewline = (iv & IDF_REDOTMATCHNL) != 0;
        break;
    case IDC_FRCOMMAND_EXEC:
        switch (static_cast<int>(iv)) {
        case kIdOk: doFindNext(e); break;
        case IDC_FINDNEXT: e.forward = true; doFindNext(e); break;   // macros from 7.5.5 - 7.8.4
        case IDC_FINDPREV: e.forward = false; doFindNext(e); break;
        case IDREPLACE: doReplace(e); break;
        case IDREPLACEALL: doReplaceAll(e); break;
        case IDC_REPLACE_OPENEDFILES: doReplaceAllInOpened(e); break;
        case IDCCOUNTALL: doCount(e); break;
        case IDCMARKALL: doMarkAll(e); break;
        case IDC_CLEAR_ALL: doClearMarks(e); break;
        case IDC_FINDALL_CURRENTFILE: doFindAllInCurrent(e); break;
        case IDC_FINDALL_OPENEDFILES: doFindAllInOpened(e); break;
        case IDD_FINDINFILES_FIND_BUTTON: doFindInFiles(e); break;
        case IDD_FINDINFILES_REPLACEINFILES: doReplaceInFiles(e); break;
        default: break;
        }
        break;
    default:
        break;
    }
}

} // namespace npp::ui
