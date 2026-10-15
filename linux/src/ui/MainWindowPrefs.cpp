// Preferences (upstream WinControls/Preference): every page upstream has
// that means something on Linux, with upstream's labels (nativeLang
// "Preference/<page>") and defaults. Changes apply at once, as upstream.
// Left out: File Association (the desktop file and MIME types do it),
// Cloud settings, system tray, rendering mode, auto-updater, dialog font and
// Dark Mode tones (GTK themes the dialogs), multi-line and pinned tabs.

#include "ui/MainWindow.h"

#include "platform/Paths.h"

#include "menuCmdID.h"

#include <algorithm>
#include <functional>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

class PrefsBuilder {
public:
    PrefsBuilder(const core::Translator& tr, std::function<void()> apply) : tr_(tr), apply_(std::move(apply)) {}

    // A page in the stack: `section` is the nativeLang element under <Preference>.
    GtkWidget* page(GtkWidget* stack, const char* section, const char* fallbackTitle, const std::string& title = {})
    {
        section_ = std::string("Preference/") + section;
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(box), 12);
        GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(sw), box);
        const std::string t = title.empty() ? core::Translator::plain(tr_.dialogAttr(section_, "title", fallbackTitle)) : title;
        gtk_stack_add_titled(GTK_STACK(stack), sw, section, t.c_str());
        box_ = box;
        return box;
    }
    std::string text(int id, const char* fallback) const
    {
        return id ? core::Translator::plain(tr_.dialog(section_, id, fallback)) : std::string(fallback);
    }
    GtkWidget* box() const { return box_; }
    void pack(GtkWidget* w) { gtk_box_pack_start(GTK_BOX(box_), w, FALSE, FALSE, 0); }

    void heading(int id, const char* fallback)
    {
        GtkWidget* l = gtk_label_new(nullptr);
        gchar* m = g_markup_printf_escaped("<b>%s</b>", text(id, fallback).c_str());
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(m);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_widget_set_margin_top(l, 8);
        pack(l);
    }
    void note(const std::string& s)
    {
        GtkWidget* l = gtk_label_new(s.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
        gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
        pack(l);
    }
    GtkWidget* check(int id, const char* fallback, bool& field, int indent = 0)
    {
        GtkWidget* c = gtk_check_button_new_with_label(text(id, fallback).c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), field);
        gtk_widget_set_margin_start(c, indent * 20);
        bind(c, "toggled", [this, c, &field] {
            field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c));
            apply_();
        });
        pack(c);
        return c;
    }
    // Radio buttons for 0..n-1 (or `values`); `set` stores the choice.
    GtkWidget* radios(int current, std::vector<std::pair<int, const char*>> labels, std::function<void(int)> set,
                      std::vector<int> values = {}, bool vertical = false, int indent = 0)
    {
        GtkWidget* row = gtk_box_new(vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, vertical ? 2 : 12);
        gtk_widget_set_margin_start(row, indent * 20);
        GtkWidget* first = nullptr;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const int v = values.empty() ? static_cast<int>(i) : values[i];
            const std::string label = text(labels[i].first, labels[i].second);
            GtkWidget* r = first ? gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(first), label.c_str())
                                 : gtk_radio_button_new_with_label(nullptr, label.c_str());
            if (!first)
                first = r;
            if (current == v)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r), TRUE);
            bind(r, "toggled", [this, r, v, set] {
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r))) {
                    set(v);
                    apply_();
                }
            });
            gtk_box_pack_start(GTK_BOX(row), r, FALSE, FALSE, 0);
        }
        pack(row);
        return row;
    }
    GtkWidget* spin(int id, const char* fallback, int& field, int lo, int hi, const char* suffix = nullptr, int scale = 1, int indent = 0)
    {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, indent * 20);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(text(id, fallback).c_str()), FALSE, FALSE, 0);
        GtkWidget* s = gtk_spin_button_new_with_range(lo, hi, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), field / scale);
        bind(s, "value-changed", [this, s, &field, scale] {
            field = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(s)) * scale;
            apply_();
        });
        gtk_box_pack_start(GTK_BOX(row), s, FALSE, FALSE, 0);
        if (suffix)
            gtk_box_pack_start(GTK_BOX(row), gtk_label_new(suffix), FALSE, FALSE, 0);
        pack(row);
        return row;
    }
    GtkWidget* entry(int id, const char* fallback, std::string& field, int width = 30, int indent = 0, int maxLen = 0)
    {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, indent * 20);
        if (id || *fallback)
            gtk_box_pack_start(GTK_BOX(row), gtk_label_new(text(id, fallback).c_str()), FALSE, FALSE, 0);
        GtkWidget* e = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(e), field.c_str());
        gtk_entry_set_width_chars(GTK_ENTRY(e), width);
        if (maxLen)
            gtk_entry_set_max_length(GTK_ENTRY(e), maxLen);
        bind(e, "changed", [this, e, &field] {
            field = gtk_entry_get_text(GTK_ENTRY(e));
            apply_();
        });
        gtk_box_pack_start(GTK_BOX(row), e, width >= 40, width >= 40, 0);
        pack(row);
        last_ = e;
        return row;
    }
    GtkWidget* lastEntry() const { return last_; }
    GtkWidget* combo(int id, const char* fallback, const std::vector<std::string>& items, int& field, int indent = 0)
    {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, indent * 20);
        if (id || *fallback)
            gtk_box_pack_start(GTK_BOX(row), gtk_label_new(text(id, fallback).c_str()), FALSE, FALSE, 0);
        GtkWidget* c = gtk_combo_box_text_new();
        for (const std::string& s : items)
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), s.c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(c), field);
        bind(c, "changed", [this, c, &field] {
            field = std::max(gtk_combo_box_get_active(GTK_COMBO_BOX(c)), 0);
            apply_();
        });
        gtk_box_pack_start(GTK_BOX(row), c, FALSE, FALSE, 0);
        pack(row);
        return row;
    }
    // `dependents` are usable only while `toggle` is on.
    void enables(GtkWidget* toggle, std::vector<GtkWidget*> dependents)
    {
        auto sync = [toggle, dependents] {
            const bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
            for (GtkWidget* d : dependents)
                gtk_widget_set_sensitive(d, on);
        };
        sync();
        bind(toggle, "toggled", sync);
    }
    void bind(GtkWidget* w, const char* signal, std::function<void()> fn)
    {
        auto* f = new std::function<void()>(std::move(fn));
        g_signal_connect_data(w, signal, G_CALLBACK(+[](GtkWidget*, gpointer p) { (*static_cast<std::function<void()>*>(p))(); }), f,
                              +[](gpointer p, GClosure*) { delete static_cast<std::function<void()>*>(p); }, GConnectFlags(0));
    }
    const core::Translator& tr() const { return tr_; }

private:
    const core::Translator& tr_;
    std::function<void()> apply_;
    std::string section_;
    GtkWidget* box_ = nullptr;
    GtkWidget* last_ = nullptr;
};

std::vector<std::string> comboItems(const core::Translator& tr, const char* section, int comboId, std::vector<std::string> fallback)
{
    std::vector<std::string> out = tr.dialogComboItems(std::string("Preference/") + section, comboId);
    if (out.size() != fallback.size())
        return fallback;
    return out;
}

} // namespace

void MainWindow::cmdPreferences()
{
    platform::Settings& st = settings_;
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr("Preference", "title", "Preferences").c_str(), GTK_WINDOW(window_),
                                                 GTK_DIALOG_MODAL,
                                                 core::Translator::toMnemonic(tr_.dialog("Preference", 6001, "Close")).c_str(),
                                                 GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 860, 600);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);
    GtkWidget* stack = gtk_stack_new();
    GtkWidget* side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
    gtk_widget_set_size_request(side, 200, -1);
    gtk_box_pack_start(GTK_BOX(hbox), side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), stack, TRUE, TRUE, 0);

    PrefsBuilder b(tr_, [this] { applySettings(); });

    // ---- General -------------------------------------------------------------
    b.page(stack, "Global", "General");
    b.heading(6123, "Localization");
    GtkWidget* langs = gtk_combo_box_text_new();
    const auto available = core::Translator::available(platform::dataDir() / "nativeLang");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(langs), "english.xml", "English");
    for (const core::Translator::Language& l : available)
        if (l.fileName != "english.xml")
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(langs), l.fileName.c_str(), l.name.c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(langs), st.localization.empty() ? "english.xml" : st.localization.c_str());
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(langs)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(langs), 0);
    b.bind(langs, "changed", [this, langs] {
        if (const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(langs)))
            setLocalization(std::string(id) == "english.xml" ? std::string() : id);
    });
    b.pack(langs);
    b.heading(6131, "Menu");
    b.check(6122, "Hide (use Alt or F10 key to toggle)", st.hideMenu);
    b.heading(6133, "Status Bar");
    b.check(6134, "Hide", st.hideStatusBar);

    // ---- Toolbar -------------------------------------------------------------
    b.page(stack, "Toolbar", "Toolbar");
    GtkWidget* tbHide = b.check(6102, "Hide", st.hideToolbar);
    GtkWidget* sets = b.radios(st.toolbarIconSet,
                               {{6103, "Fluent UI: small"}, {6104, "Fluent UI: large"}, {6129, "Filled Fluent UI: small"},
                                {6130, "Filled Fluent UI: large"}, {6105, "Standard icons: small"}},
                               [&st](int v) { st.toolbarIconSet = v; }, {}, true, 1);
    g_signal_connect_swapped(tbHide, "toggled", G_CALLBACK(+[](GtkWidget* r) {
                                 gtk_widget_set_sensitive(r, !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
                                                                 g_object_get_data(G_OBJECT(r), "npp-hide"))));
                             }), sets);
    g_object_set_data(G_OBJECT(sets), "npp-hide", tbHide);
    gtk_widget_set_sensitive(sets, !st.hideToolbar);

    // ---- Tab Bar -------------------------------------------------------------
    b.page(stack, "Tabbar", "Tab Bar");
    b.heading(6126, "Look && feel");
    b.check(6107, "Reduce", st.tabReduce);
    b.check(6109, "Change inactive tab color", st.tabInactiveColor);
    b.check(6110, "Draw a colored bar on active tab", st.tabActiveBar);
    GtkWidget* closeBtn = b.check(6112, "Show close button", st.tabCloseButton);
    GtkWidget* inactiveBtn = b.check(6111, "Show buttons on inactive tabs", st.tabButtonsOnInactive, 1);
    b.enables(closeBtn, {inactiveBtn});
    b.spin(6136, "Max. tab label length:", st.tabMaxLabel, 0, 256, "(0 = no limit)");
    b.heading(6125, "Behavior");
    GtkWidget* hideTabs = b.check(6118, "Hide", st.tabHide);
    GtkWidget* vertical = b.check(6120, "Vertical", st.tabVertical);
    GtkWidget* lock = b.check(6108, "Lock (no drag and drop)", st.tabLock);
    GtkWidget* dbl = b.check(6113, "Double click to close document", st.tabDoubleClickClose);
    b.check(6121, "Exit on close the last tab", st.tabExitOnLast);
    b.bind(hideTabs, "toggled", [hideTabs, vertical, lock, dbl] {
        for (GtkWidget* w : {vertical, lock, dbl})
            gtk_widget_set_sensitive(w, !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hideTabs)));
    });
    for (GtkWidget* w : {vertical, lock, dbl})
        gtk_widget_set_sensitive(w, !st.tabHide);

    // ---- Editing 1 -----------------------------------------------------------
    b.page(stack, "Scintillas", "Editing 1");
    b.heading(6216, "Caret Settings");
    b.spin(6217, "Width:", st.caretWidth, 0, 3, "(0 = block)");
    b.spin(6219, "Blink rate:", st.caretBlinkMs, 0, 25, "× 100 ms (0 = no blinking)", 100);
    b.heading(6651, "Current Line Indicator");
    b.radios(st.currentLineMode, {{6652, "None"}, {6653, "Highlight Background"}, {6654, "Frame"}}, [&st](int v) {
        st.currentLineMode = v;
        st.currentLineHighlight = v != 0;
    });
    b.spin(6655, "Width:", st.currentLineFrame, 1, 6, nullptr, 1, 1);
    b.heading(6227, "Line Wrap");
    b.radios(st.wrapIndent, {{6228, "Default"}, {6229, "Aligned"}, {6230, "Indent"}}, [&st](int v) { st.wrapIndent = v; });
    b.heading(0, "");
    b.check(6215, "Enable smooth font", st.smoothFont);
    b.check(6236, "Enable scrolling beyond last line", st.scrollBeyondLastLine);
    b.check(6239, "Keep selection when right-click outside of selection", st.keepSelOnRightClick);
    b.check(6245, "Enable virtual space", st.virtualSpace);
    b.check(6214, "Enable Copy/Cut Line without selection", st.copyLineNoSelection);
    b.check(6266, "Disable selected text drag-and-drop", st.noSelDragDrop);
    b.check(6225, "Apply custom color to selected text foreground", st.selectedTextFg);
    b.check(6246, "Make current level folding/unfolding commands toggleable", st.foldToggle);

    // ---- Editing 2 -----------------------------------------------------------
    b.page(stack, "Scintillas2", "Editing 2");
    b.heading(6521, "Multi-Editing");
    GtkWidget* multi = b.check(6522, "Enable Multi-Editing (Ctrl+Mouse click/selection)", st.multiEditing);
    GtkWidget* colMulti = b.check(6523, "Enable Column Selection to Multi-Editing", st.columnToMultiEdit);
    b.enables(multi, {colMulti});
    b.heading(6247, "EOL (CRLF)");
    b.radios(st.eolDisplay, {{6248, "Default"}, {6249, "Plain Text"}}, [&st](int v) { st.eolDisplay = v; });
    b.check(6250, "Custom Color", st.eolCustomColor);
    b.note("The custom colours are the theme's \"EOL custom color\" and \"Non-printing characters custom color\" (Style Configurator, Global Styles). "
           "View > Show Symbol shows these characters.");
    b.heading(6252, "Non-Printing Characters");
    b.radios(st.npcDisplay, {{6254, "Abbreviation"}, {6255, "Codepoint"}}, [&st](int v) { st.npcDisplay = v; });
    b.check(6256, "Custom Color", st.npcCustomColor);
    b.check(6258, "Apply Appearance settings to C0, C1 && Unicode EOL", st.npcIncludeCcUniEol);
    b.check(6259, "Prevent control character (C0 code) typing into document", st.preventC0);

    // ---- Dark Mode -------------------------------------------------------------
    b.page(stack, "DarkMode", "Dark Mode");
    b.radios(st.darkMode, {{7131, "Light mode"}, {7132, "Dark mode"}, {7133, "Follow Windows"}},
             [this](int v) {
                 settings_.darkMode = v;
                 applyDarkMode();
             },
             {}, true);
    b.note("\"Follow\" follows the desktop's light or dark style. Dark mode switches to the theme below and back to the "
           "previous theme in light mode; the GTK widgets follow the editor theme.");
    {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Dark theme:"), FALSE, FALSE, 0);
        GtkWidget* themes = gtk_combo_box_text_new();
        for (const std::string& t : availableThemes())
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(themes), t.c_str(), fs::path(t).stem().c_str());
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(themes), st.darkTheme.c_str());
        b.bind(themes, "changed", [this, themes] {
            if (const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(themes))) {
                settings_.darkTheme = id;
                applyDarkMode();
            }
        });
        gtk_box_pack_start(GTK_BOX(row), themes, FALSE, FALSE, 0);
        b.pack(row);
    }

    // ---- Margins/Border/Edge -----------------------------------------------------
    b.page(stack, "MarginsBorderEdge", "Margins/Border/Edge");
    b.heading(6201, "Fold Margin Style");
    b.radios(st.foldStyle, {{6202, "Simple"}, {6203, "Arrow"}, {6204, "Circle tree"}, {6205, "Box tree"}, {6226, "None"}},
             [&st](int v) { st.foldStyle = v; });
    b.heading(6291, "Line Number");
    GtkWidget* ln = b.check(6206, "Display", st.showLineNumbers);
    GtkWidget* lnw = b.radios(st.lineNumberDynamic ? 0 : 1, {{6292, "Dynamic width"}, {6293, "Constant width"}},
                              [&st](int v) { st.lineNumberDynamic = v == 0; }, {}, false, 1);
    b.enables(ln, {lnw});
    b.check(6207, "Display bookmark", st.showBookmarkMargin);
    b.heading(6295, "Change History");
    b.check(6223, "Show in the margin", st.changeHistoryMargin);
    b.check(6296, "Show in the text", st.changeHistoryText);
    b.heading(6211, "Vertical Edge Settings");
    b.radios(st.edgeMode, {{6235, "No edge"}, {0, "Line mode"}, {6213, "Background mode"}}, [&st](int v) { st.edgeMode = v; });
    b.entry(0, "Columns:", st.edgeColumns, 30, 1);
    b.note("One or more column numbers separated by spaces (line mode); background mode uses the first one.");
    b.heading(6208, "Padding");
    b.spin(6209, "Left", st.paddingLeft, 0, 30, "px");
    b.spin(6210, "Right", st.paddingRight, 0, 30, "px");

    // ---- New Document ----------------------------------------------------------
    b.page(stack, "NewDoc", "New Document");
    b.heading(6401, "Format (Line ending)");
    b.radios(st.newDocEol, {{6402, "Windows (CR LF)"}, {6403, "Unix (LF)"}, {6404, "Macintosh (CR)"}}, [&st](int v) { st.newDocEol = v; },
             {SC_EOL_CRLF, SC_EOL_LF, SC_EOL_CR});
    b.heading(6405, "Encoding");
    b.radios(st.newDocEncoding, {{6407, "UTF-8"}, {6408, "UTF-8 with BOM"}, {6409, "UTF-16 Big Endian with BOM"}, {6410, "UTF-16 Little Endian with BOM"}},
             [&st](int v) { st.newDocEncoding = v; }, {}, true);
    b.heading(6411, "Default language:");
    GtkWidget* deflang = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(deflang), "", tr_.command(IDM_LANG_TEXT, "Normal Text").c_str());
    for (const core::Language& l : langs_.all())
        if (!l.isPlainText() && !l.name.empty())
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(deflang), l.name.c_str(), (l.shortName.empty() ? l.name : l.shortName).c_str());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(deflang), st.newDocLanguage.c_str());
    b.bind(deflang, "changed", [&st, deflang] {
        const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(deflang));
        st.newDocLanguage = id ? id : "";
    });
    b.pack(deflang);
    b.heading(6419, "New Document");
    b.check(6432, "Always open a new document in addition at startup", st.newDocAtStartup);
    b.check(6433, "Use the first line of document as untitled tab name", st.firstLineAsTabName);

    // ---- Default Directory -------------------------------------------------------
    b.page(stack, "DefaultDir", "Default Directory");
    b.heading(6413, "Default Open/Save file Directory");
    b.radios(st.defaultDirMode, {{6414, "Follow current document"}, {6415, "Remember last used directory"}, {0, "This directory:"}},
             [&st](int v) { st.defaultDirMode = v; }, {}, true);
    {
        GtkWidget* dir = gtk_file_chooser_button_new("Default directory", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        if (!st.defaultDir.empty())
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dir), st.defaultDir.c_str());
        gtk_widget_set_margin_start(dir, 20);
        b.bind(dir, "file-set", [&st, dir] {
            gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dir));
            st.defaultDir = fn ? fn : "";
            g_free(fn);
        });
        b.pack(dir);
    }
    b.heading(0, "");
    b.check(6431, "Open all files of folder instead of launching Folder as Workspace on folder dropping", st.dropFolderOpensFiles);

    // ---- Language ----------------------------------------------------------------
    b.page(stack, "Language", "Language");
    b.heading(6508, "Language Menu");
    {
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        GtkListStore* stores[2] = {gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING), gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING)};
        auto hidden = [&st](const std::string& n) { return std::find(st.langHidden.begin(), st.langHidden.end(), n) != st.langHidden.end(); };
        for (const core::Language& l : langs_.all())
            if (!l.isPlainText() && !l.shortName.empty())
                gtk_list_store_insert_with_values(stores[hidden(l.name) ? 1 : 0], nullptr, -1, 0, l.shortName.c_str(), 1, l.name.c_str(), -1);
        GtkWidget* views[2];
        const std::string titles[2] = {b.text(6505, "Available items"), b.text(6506, "Disabled items")};
        for (int i = 0; i < 2; ++i) {
            GtkTreeModel* sorted = gtk_tree_model_sort_new_with_model(GTK_TREE_MODEL(stores[i]));
            gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(sorted), 0, GTK_SORT_ASCENDING);
            views[i] = gtk_tree_view_new_with_model(sorted);
            g_object_unref(sorted);
            g_object_unref(stores[i]);
            gtk_tree_view_append_column(GTK_TREE_VIEW(views[i]),
                                        gtk_tree_view_column_new_with_attributes(titles[i].c_str(), gtk_cell_renderer_text_new(), "text", 0, nullptr));
            gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(views[i])), GTK_SELECTION_MULTIPLE);
            GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
            gtk_widget_set_size_request(sw, 200, 260);
            gtk_container_add(GTK_CONTAINER(sw), views[i]);
            gtk_grid_attach(GTK_GRID(grid), sw, i * 2, 0, 1, 1);
        }
        GtkWidget* arrows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_valign(arrows, GTK_ALIGN_CENTER);
        GtkWidget* toHidden = gtk_button_new_with_label("→");
        GtkWidget* toShown = gtk_button_new_with_label("←");
        gtk_box_pack_start(GTK_BOX(arrows), toHidden, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(arrows), toShown, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), arrows, 1, 0, 1, 1);
        auto move = [this, views](int from) {
            GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(views[from]));
            GtkTreeModel* sorted = nullptr;
            GList* rows = gtk_tree_selection_get_selected_rows(sel, &sorted);
            GtkTreeModel* fromStore = gtk_tree_model_sort_get_model(GTK_TREE_MODEL_SORT(sorted));
            GtkTreeModel* toStore = gtk_tree_model_sort_get_model(GTK_TREE_MODEL_SORT(gtk_tree_view_get_model(GTK_TREE_VIEW(views[1 - from]))));
            std::vector<GtkTreeRowReference*> refs;
            for (GList* l = rows; l; l = l->next) {
                GtkTreePath* child = gtk_tree_model_sort_convert_path_to_child_path(GTK_TREE_MODEL_SORT(sorted), static_cast<GtkTreePath*>(l->data));
                refs.push_back(gtk_tree_row_reference_new(fromStore, child));
                gtk_tree_path_free(child);
            }
            g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
            for (GtkTreeRowReference* r : refs) {
                GtkTreePath* p = gtk_tree_row_reference_get_path(r);
                GtkTreeIter it;
                if (p && gtk_tree_model_get_iter(fromStore, &it, p)) {
                    gchar *label = nullptr, *name = nullptr;
                    gtk_tree_model_get(fromStore, &it, 0, &label, 1, &name, -1);
                    gtk_list_store_insert_with_values(GTK_LIST_STORE(toStore), nullptr, -1, 0, label, 1, name, -1);
                    auto& h = settings_.langHidden;
                    if (from == 0)
                        h.push_back(name);
                    else
                        h.erase(std::remove(h.begin(), h.end(), std::string(name)), h.end());
                    g_free(label);
                    g_free(name);
                    gtk_list_store_remove(GTK_LIST_STORE(fromStore), &it);
                }
                gtk_tree_path_free(p);
                gtk_tree_row_reference_free(r);
            }
            applySettings();
        };
        b.bind(toHidden, "clicked", [move] { move(0); });
        b.bind(toShown, "clicked", [move] { move(1); });
        b.pack(grid);
    }
    b.check(6507, "Make language menu compact", st.langMenuCompact);
    b.heading(0, "");
    b.check(6335, "Treat backslash as escape character for SQL", st.sqlBackslashEscape);

    // ---- Indentation ----------------------------------------------------------------
    b.page(stack, "Indentation", "Indentation");
    b.heading(6301, "Indent Settings");
    {
        // Upstream: "[Default]" and the languages; a language can override the default.
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkListStore* store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
        gtk_list_store_insert_with_values(store, nullptr, -1, 0, "[Default]", 1, "", -1);
        for (const core::Language& l : langs_.all())
            if (!l.isPlainText() && !l.shortName.empty())
                gtk_list_store_insert_with_values(store, nullptr, -1, 0, l.shortName.c_str(), 1, l.name.c_str(), -1);
        GtkWidget* list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
        g_object_unref(store);
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(list), FALSE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(list), gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", 0, nullptr));
        GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_widget_set_size_request(sw, 180, 220);
        gtk_container_add(GTK_CONTAINER(sw), list);
        gtk_box_pack_start(GTK_BOX(row), sw, FALSE, FALSE, 0);
        GtkWidget* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget* useDefault = gtk_check_button_new_with_label(b.text(6510, "Use default value").c_str());
        GtkWidget* sizeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(sizeRow), gtk_label_new(b.text(6303, "Indent size:").c_str()), FALSE, FALSE, 0);
        GtkWidget* size = gtk_spin_button_new_with_range(1, 64, 1);
        gtk_box_pack_start(GTK_BOX(sizeRow), size, FALSE, FALSE, 0);
        GtkWidget* usingLabel = gtk_label_new(b.text(6310, "Indent using:").c_str());
        gtk_label_set_xalign(GTK_LABEL(usingLabel), 0);
        GtkWidget* tabs = gtk_radio_button_new_with_label(nullptr, b.text(6311, "Tab character").c_str());
        GtkWidget* spaces = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(tabs), b.text(6302, "Space character(s)").c_str());
        for (GtkWidget* w : {useDefault, sizeRow, usingLabel, tabs, spaces})
            gtk_box_pack_start(GTK_BOX(right), w, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), right, TRUE, TRUE, 0);
        b.pack(row);

        struct IndentUi {
            MainWindow* w;
            GtkWidget *list, *useDefault, *size, *tabs, *spaces;
            std::string lang;
            bool loading = false;
            void load()
            {
                loading = true;
                platform::Settings& s = w->settings_;
                int sz = s.tabSize;
                bool sp = s.replaceTabsBySpace;
                const auto it = s.langIndent.find(lang);
                const bool own = !lang.empty() && it != s.langIndent.end();
                int spi = sp ? 1 : 0;
                if (own && std::sscanf(it->second.c_str(), "%d,%d", &sz, &spi) == 2)
                    sp = spi != 0;
                gtk_widget_set_visible(useDefault, !lang.empty());
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(useDefault), !own);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(size), sz);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sp ? spaces : tabs), TRUE);
                for (GtkWidget* x : {size, tabs, spaces})
                    gtk_widget_set_sensitive(x, lang.empty() || own);
                loading = false;
            }
            void store()
            {
                if (loading)
                    return;
                platform::Settings& s = w->settings_;
                const int sz = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(size));
                const bool sp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(spaces));
                if (lang.empty()) {
                    s.tabSize = sz;
                    s.replaceTabsBySpace = sp;
                } else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(useDefault))) {
                    s.langIndent.erase(lang);
                } else {
                    s.langIndent[lang] = std::to_string(sz) + "," + (sp ? "1" : "0");
                }
                load();
                w->applySettings();
            }
        };
        auto* ui = new IndentUi{this, list, useDefault, size, tabs, spaces, {}};
        g_object_set_data_full(G_OBJECT(list), "npp-indent", ui, +[](gpointer p) { delete static_cast<IndentUi*>(p); });
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(gtk_tree_view_get_model(GTK_TREE_VIEW(list))), &first))
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), &first);
        g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(list)), "changed", G_CALLBACK(+[](GtkTreeSelection* sel, gpointer p) {
                             auto* x = static_cast<IndentUi*>(p);
                             GtkTreeModel* m = nullptr;
                             GtkTreeIter it;
                             if (!gtk_tree_selection_get_selected(sel, &m, &it))
                                 return;
                             gchar* name = nullptr;
                             gtk_tree_model_get(m, &it, 1, &name, -1);
                             x->lang = name ? name : "";
                             g_free(name);
                             x->load();
                         }), ui);
        for (GtkWidget* w : {useDefault, tabs, spaces})
            g_signal_connect_swapped(w, "toggled", G_CALLBACK(+[](IndentUi* x) { x->store(); }), ui);
        g_signal_connect_swapped(size, "value-changed", G_CALLBACK(+[](IndentUi* x) { x->store(); }), ui);
        ui->load();
    }
    b.check(6512, "Backspace key unindents instead of removing single space", st.backspaceUnindent);
    b.heading(7161, "Auto-indent");
    b.radios(st.autoIndentMode, {{7162, "None"}, {7163, "Basic"}, {7164, "Advanced"}}, [&st](int v) {
        st.autoIndentMode = v;
        st.autoIndent = v != 0;
    });

    // ---- Highlighting ------------------------------------------------------------------
    b.page(stack, "Highlighting", "Highlighting");
    b.heading(6351, "Style All Occurrences of Token");
    b.check(6352, "Match case", st.markMatchCase);
    b.check(6353, "Match whole word only", st.markWholeWord);
    b.heading(6333, "Smart Highlighting");
    GtkWidget* smart = b.check(6326, "Enable", st.smartHighlight);
    GtkWidget* smartOther = b.check(6340, "Highlight another view", st.smartOtherView, 1);
    GtkWidget* smartFind = b.check(6339, "Use Find dialog settings", st.smartUseFind, 1);
    GtkWidget* smartCase = b.check(6332, "Match case", st.smartMatchCase, 2);
    GtkWidget* smartWord = b.check(6338, "Match whole word only", st.smartWholeWord, 2);
    b.enables(smart, {smartOther, smartFind, smartCase, smartWord});
    b.bind(smartFind, "toggled", [smart, smartFind, smartCase, smartWord] {
        const bool on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(smart)) && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(smartFind));
        for (GtkWidget* w : {smartCase, smartWord})
            gtk_widget_set_sensitive(w, on);
    });
    if (st.smartUseFind)
        for (GtkWidget* w : {smartCase, smartWord})
            gtk_widget_set_sensitive(w, FALSE);
    b.heading(6329, "Highlight Matching Tags");
    GtkWidget* tags = b.check(6327, "Enable", st.tagMatch);
    GtkWidget* attrs = b.check(6328, "Highlight tag attributes", st.tagAttributes, 1);
    GtkWidget* zones = b.check(6330, "Highlight comment/php/asp zone", st.tagZones, 1);
    b.enables(tags, {attrs, zones});

    // ---- Print -------------------------------------------------------------------------
    b.page(stack, "Print", "Print");
    b.check(6601, "Print line number", st.printLineNumbers);
    b.heading(6602, "Color Options");
    b.radios(st.printColorMode, {{6603, "WYSIWYG"}, {6604, "Invert"}, {6605, "Black on white"}, {6606, "No background color"}},
             [&st](int v) { st.printColorMode = v; }, {}, true);
    b.heading(6607, "Margin Settings (Unit:mm)");
    b.spin(6612, "Left", st.printMarginLeft, 0, 100);
    b.spin(6613, "Top", st.printMarginTop, 0, 100);
    b.spin(6614, "Right", st.printMarginRight, 0, 100);
    b.spin(6615, "Bottom", st.printMarginBottom, 0, 100);
    b.heading(6728, "Header and Footer");
    GtkWidget* lastFocus = nullptr;
    std::vector<GtkWidget*> hfEntries;
    auto hf = [&](int id, const char* fb, std::string& field) {
        b.entry(id, fb, field, 40, 1);
        hfEntries.push_back(b.lastEntry());
    };
    b.heading(6708, "Header");
    hf(6709, "Left part", st.headerLeft);
    hf(6710, "Middle part", st.headerMiddle);
    hf(6711, "Right part", st.headerRight);
    b.check(6706, "Bold", st.headerBold, 1);
    b.check(6707, "Italic", st.headerItalic, 1);
    b.heading(6719, "Footer");
    hf(6720, "Left part", st.footerLeft);
    hf(6721, "Middle part", st.footerMiddle);
    hf(6722, "Right part", st.footerRight);
    b.check(6717, "Bold", st.footerBold, 1);
    b.check(6718, "Italic", st.footerItalic, 1);
    {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(b.text(6725, "Variable:").c_str()), FALSE, FALSE, 0);
        GtkWidget* vars = gtk_combo_box_text_new();
        static const char* const kVars[] = {"$(FULL_CURRENT_PATH)", "$(FILE_NAME)", "$(CURRENT_DIRECTORY)", "$(CURRENT_PRINTING_PAGE)",
                                            "$(SHORT_DATE)", "$(LONG_DATE)", "$(TIME)"};
        const auto names = comboItems(tr_, "Print", 6724, {"Full file name path", "File name", "File directory", "Page",
                                                           "Short date format", "Long date format", "Time"});
        for (std::size_t i = 0; i < names.size(); ++i)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(vars), kVars[i], names[i].c_str());
        gtk_combo_box_set_active(GTK_COMBO_BOX(vars), 0);
        GtkWidget* add = gtk_button_new_with_label(b.text(6723, "Add").c_str());
        gtk_box_pack_start(GTK_BOX(row), vars, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), add, FALSE, FALSE, 0);
        b.pack(row);
        auto* target = new GtkWidget*(hfEntries.front());
        g_object_set_data_full(G_OBJECT(add), "npp-target", target, +[](gpointer p) { delete static_cast<GtkWidget**>(p); });
        for (GtkWidget* e : hfEntries)
            g_signal_connect(e, "focus-in-event", G_CALLBACK(+[](GtkWidget* w, GdkEvent*, gpointer p) -> gboolean {
                                 *static_cast<GtkWidget**>(p) = w;
                                 return FALSE;
                             }), target);
        g_object_set_data(G_OBJECT(add), "npp-vars", vars);
        g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer) {
                             GtkWidget* e = *static_cast<GtkWidget**>(g_object_get_data(G_OBJECT(btn), "npp-target"));
                             const char* v = gtk_combo_box_get_active_id(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(btn), "npp-vars")));
                             if (!e || !v)
                                 return;
                             gint pos = gtk_editable_get_position(GTK_EDITABLE(e));
                             gtk_editable_insert_text(GTK_EDITABLE(e), v, -1, &pos);
                         }), nullptr);
        (void)lastFocus;
    }

    // ---- Searching -----------------------------------------------------------------------
    b.page(stack, "Searching", "Searching");
    b.heading(6907, "When Find Dialog is Invoked");
    GtkWidget* fill = b.check(6908, "Fill Find Field with Selected Text", st.fillFindWithSelection);
    GtkWidget* fillMax = b.spin(0, "Max Characters to Auto-Fill Find Field from Selection:", st.fillFindMaxChars, 1, 16383, nullptr, 1, 1);
    GtkWidget* word = b.check(6909, "Select Word Under Caret when Nothing Selected", st.selectWordUnderCaret, 1);
    b.enables(fill, {fillMax, word});
    b.spin(6910, "Minimum Size for Auto-Checking \"In selection\":", st.inSelectionThreshold, 0, 1000000);
    b.check(6913, "Fill Find in Files Directory Field Based On Active Document", st.fifDirFromDoc);
    b.heading(0, "");
    b.check(6902, "Use monospaced font in Find dialog", st.findMonospace);
    b.check(6903, "Find dialog remains open after search that outputs to results window", st.findStaysOpen);
    b.check(6904, "Confirm Replace All in All Opened Documents", st.confirmReplaceAllDocs);
    b.check(6905, "Replace: Don't move to the following occurrence", st.replaceNoMove);
    b.check(6906, "Search Result window: show only one entry per found line if possible", st.oneEntryPerLine);

    // ---- Recent Files History ------------------------------------------------------
    b.page(stack, "RecentFilesHistory", "Recent Files History");
    b.heading(6304, "Recent Files History");
    bool dontCheck = !st.recentCheckAtLaunch;
    GtkWidget* dc = b.check(6305, "Don't check at launch time", dontCheck);
    b.bind(dc, "toggled", [&st, dc] { st.recentCheckAtLaunch = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dc)); });
    b.spin(6306, "Max. number of entries:", st.maxRecentFiles, 0, 30);
    b.heading(6429, "Display");
    b.check(6424, "In Submenu", st.recentSubmenu);
    b.radios(st.recentDisplay, {{6425, "Only File Name"}, {6426, "Full File Name Path"}, {6427, "Customize Maximum Length:"}},
             [&st](int v) { st.recentDisplay = v; }, {}, true);
    b.spin(0, "", st.recentMaxLen, 10, 1000, "characters", 1, 1);

    // ---- Backup ----------------------------------------------------------------------
    b.page(stack, "Backup", "Backup");
    b.heading(6817, "Session snapshot and periodic backup");
    GtkWidget* remember = b.check(6309, "Remember current session for next launch", st.rememberSession);
    GtkWidget* absent = b.check(6825, "Remember inaccessible files from past session", st.keepAbsentSessionFiles, 1);
    GtkWidget* snap = b.check(6818, "Enable session snapshot and periodic backup", st.snapshot, 1);
    GtkWidget* every = b.spin(6819, "Trigger backup on modification in every", st.snapshotIntervalMs, 1, 3600, b.text(6821, "seconds").c_str(), 1000, 2);
    b.enables(remember, {absent, snap, every});
    {
        GtkWidget* l = gtk_label_new((b.text(6822, "Backup path:") + " " + (platform::userConfigDir() / "backup").string()).c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_selectable(GTK_LABEL(l), TRUE);
        gtk_widget_set_margin_start(l, 40);
        b.pack(l);
    }
    b.heading(6801, "Backup on save");
    b.radios(static_cast<int>(st.saveBackup), {{6315, "None"}, {6316, "Simple backup"}, {6317, "Verbose backup"}},
             [&st](int v) { st.saveBackup = static_cast<platform::SaveBackup>(v); });
    {
        GtkWidget* dirRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(dirRow), gtk_label_new(b.text(6804, "Custom Backup Directory").c_str()), FALSE, FALSE, 0);
        GtkWidget* dir = gtk_file_chooser_button_new("Backup directory", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        if (!st.saveBackupDir.empty())
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dir), st.saveBackupDir.c_str());
        b.bind(dir, "file-set", [&st, dir] {
            gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dir));
            st.saveBackupDir = fn ? fn : "";
            g_free(fn);
        });
        GtkWidget* clearDir = gtk_button_new_with_label("Next to the file");
        b.bind(clearDir, "clicked", [&st, dir] {
            st.saveBackupDir.clear();
            gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(dir));
        });
        gtk_box_pack_start(GTK_BOX(dirRow), dir, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(dirRow), clearDir, FALSE, FALSE, 0);
        b.pack(dirRow);
    }

    // ---- Auto-Completion ------------------------------------------------------------------
    b.page(stack, "AutoCompletion", "Auto-Completion");
    b.heading(6807, "Auto-Completion");
    GtkWidget* ac = b.check(6808, "Enable auto-completion on each input", st.acEnable);
    GtkWidget* acMode = b.radios(st.acMode, {{6809, "Function completion"}, {6810, "Word completion"}, {6816, "Function and word completion"}},
                                 [&st](int v) { st.acMode = v; }, {}, true, 1);
    GtkWidget* acFrom = b.spin(6811, "From", st.acFromChar, 1, 9, b.text(6813, "th character").c_str(), 1, 1);
    GtkWidget* acIgnore = b.check(6824, "Ignore numbers", st.acIgnoreNumbers, 1);
    GtkWidget* acBrief = b.check(6872, "Make auto-completion list brief", st.acBrief, 1);
    b.enables(ac, {acMode, acFrom, acIgnore, acBrief});
    b.heading(6869, "Insert Selection");
    b.check(6870, "TAB", st.acInsertTab);
    b.check(6871, "ENTER", st.acInsertEnter);
    b.heading(0, "");
    b.check(6815, "Function parameters hint on input", st.acParamHint);
    b.heading(6851, "Auto-Insert");
    {
        GtkWidget* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        int col = 0;
        auto pairBox = [&](const char* label, bool& field) {
            GtkWidget* c = gtk_check_button_new_with_label(label);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), field);
            b.bind(c, "toggled", [c, &field] { field = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c)); });
            gtk_grid_attach(GTK_GRID(grid), c, col++, 0, 1, 1);
        };
        pairBox("( )", st.insertParen);
        pairBox("[ ]", st.insertBracket);
        pairBox("{ }", st.insertBrace);
        pairBox("\" \"", st.insertDQuote);
        pairBox("' '", st.insertSQuote);
        b.pack(grid);
    }
    b.check(6857, "html/xml close tag", st.insertCloseTag);
    for (int i = 0; i < 3; ++i) {
        static const int ids[] = {6860, 6863, 6866};
        static const char* const fbs[] = {"Matched pair 1:", "Matched pair 2:", "Matched pair 3:"};
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new(b.text(ids[i], fbs[i]).c_str()), FALSE, FALSE, 0);
        for (int side = 0; side < 2; ++side) {
            std::string& field = side ? st.pairClose[static_cast<std::size_t>(i)] : st.pairOpen[static_cast<std::size_t>(i)];
            gtk_box_pack_start(GTK_BOX(row), gtk_label_new(b.text(side ? 6859 : 6858, side ? "Close" : "Open").c_str()), FALSE, FALSE, 0);
            GtkWidget* e = gtk_entry_new();
            gtk_entry_set_max_length(GTK_ENTRY(e), 1);
            gtk_entry_set_width_chars(GTK_ENTRY(e), 2);
            gtk_entry_set_text(GTK_ENTRY(e), field.c_str());
            b.bind(e, "changed", [e, &field] { field = gtk_entry_get_text(GTK_ENTRY(e)); });
            gtk_box_pack_start(GTK_BOX(row), e, FALSE, FALSE, 0);
        }
        b.pack(row);
    }

    // ---- Multi-Instance & Date -------------------------------------------------------------
    b.page(stack, "MultiInstance", "Multi-Instance & Date");
    b.heading(6151, "Multi-instance settings *");
    b.radios(st.multiInstMode,
             {{6154, "Default (mono-instance)"}, {6152, "Open session in a new instance (and save session automatically on exit)"},
              {6153, "Always in multi-instance mode"}},
             [&st](int v) { st.multiInstMode = v; }, {}, true);
    b.note(b.text(6155, "* The modification of this setting needs to restart Notepad++"));
    b.heading(6171, "Customize insert Date Time");
    b.check(6175, "Reverse default date time order (short && long formats)", st.dateReverse);
    GtkWidget* fmtRow = b.entry(6172, "Custom format:", st.dateFormat, 30);
    {
        GtkWidget* preview = gtk_label_new(formatCustomDate(st.dateFormat).c_str());
        gtk_label_set_xalign(GTK_LABEL(preview), 0);
        gtk_widget_set_margin_start(preview, 20);
        b.pack(preview);
        GtkWidget* e = b.lastEntry();
        b.bind(e, "changed", [preview, e] { gtk_label_set_text(GTK_LABEL(preview), formatCustomDate(gtk_entry_get_text(GTK_ENTRY(e))).c_str()); });
        b.note("Edit > Insert > Date Time (customized). Windows format: yyyy yy MMMM MMM MM M dddd ddd dd d HH H hh h mm m ss s tt t, text in '…'.");
    }
    (void)fmtRow;
    b.heading(6181, "Panel State and [-nosession] *");
    b.note(b.text(6182, "Remember panel state (panel is open) in other instances (multi-instance mode) or when using command line parameter [-nosession]"));
    b.check(6183, "Clipboard History", st.noSessionClipboard, 1);
    b.check(6184, "Document List", st.noSessionDocList, 1);
    b.check(6185, "Character Panel", st.noSessionCharPanel, 1);
    b.check(6186, "Folder as Workspace", st.noSessionFolder, 1);
    b.check(6187, "Project Panels", st.noSessionProjects, 1);
    b.check(6188, "Document Map", st.noSessionDocMap, 1);
    b.check(6189, "Function List", st.noSessionFuncList, 1);
    b.check(6190, "Plugin Panels", st.noSessionPlugins, 1);

    // ---- Delimiter ---------------------------------------------------------------------------
    b.page(stack, "Delimiter", "Delimiter");
    b.heading(6251, "Delimiter selection settings (Ctrl + Mouse double click)");
    {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        for (int side = 0; side < 2; ++side) {
            std::string& field = side ? st.delimClose : st.delimOpen;
            gtk_box_pack_start(GTK_BOX(row), gtk_label_new(b.text(side ? 6255 : 6252, side ? "Close" : "Open").c_str()), FALSE, FALSE, 0);
            GtkWidget* e = gtk_entry_new();
            gtk_entry_set_max_length(GTK_ENTRY(e), 1);
            gtk_entry_set_width_chars(GTK_ENTRY(e), 2);
            gtk_entry_set_text(GTK_ENTRY(e), field.c_str());
            b.bind(e, "changed", [e, &field] { field = gtk_entry_get_text(GTK_ENTRY(e)); });
            gtk_box_pack_start(GTK_BOX(row), e, FALSE, FALSE, 0);
        }
        b.pack(row);
    }
    b.check(6256, "Allow on several lines", st.delimMultiLine);
    b.heading(6161, "Word character list");
    b.radios(st.extraWordCharsOn ? 1 : 0,
             {{6162, "Use default Word character list as it is"},
              {6163, "Add your character as part of word (don't choose it unless you know what you're doing)"}},
             [&st](int v) { st.extraWordCharsOn = v == 1; }, {}, true);
    b.entry(0, "", st.extraWordChars, 30, 1);

    // ---- Performance ---------------------------------------------------------------------------
    b.page(stack, "Performance", "Performance");
    b.heading(7141, "Large File Restriction");
    GtkWidget* large = b.check(7143, "Enable Large File Restriction (no syntax highlighting)", st.largeFileRestrict);
    GtkWidget* size = b.spin(7144, "Define Large File Size:", st.largeFileMB, 1, 2046, b.text(7146, "MB   (1 - 2046)").c_str(), 1, 1);
    GtkWidget* l1 = b.check(7147, "Allow Brace Match", st.largeAllowBrace, 1);
    GtkWidget* l2 = b.check(7148, "Allow Auto-Completion", st.largeAllowAC, 1);
    GtkWidget* l3 = b.check(7149, "Allow Smart Highlighting", st.largeAllowSmart, 1);
    GtkWidget* l4 = b.check(7150, "Deactivate Word Wrap globally", st.largeNoWrap, 1);
    GtkWidget* l5 = b.check(7151, "Allow URL Clickable Link", st.largeAllowLinks, 1);
    b.enables(large, {size, l1, l2, l3, l4, l5});
    b.note("Applies to files opened from now on.");

    // ---- Clickable links (upstream "Cloud & Link") ------------------------------------------------
    b.page(stack, "Cloud", "Cloud & Link", core::Translator::plain(tr_.dialog("Preference/Cloud", 6318, "Clickable Link Settings")));
    b.heading(6318, "Clickable Link Settings");
    GtkWidget* links = b.check(6319, "Enable", st.linkEnable);
    GtkWidget* underline = b.check(6320, "No underline", st.linkNoUnderline, 1);
    GtkWidget* box = b.check(6350, "Enable fullbox mode", st.linkFullBox, 1);
    GtkWidget* schemes = b.entry(6264, "URI customized schemes:", st.linkSchemes, 60, 1);
    b.enables(links, {underline, box, schemes});
    b.note("Click a link to open it. http(s), ftp, file and mailto are always recognised.");

    // ---- Search Engine ----------------------------------------------------------------------------
    b.page(stack, "SearchEngine", "Search Engine");
    b.heading(6271, "Search Engine (for command \"Search on Internet\")");
    b.radios(st.searchEngine, {{6272, "DuckDuckGo"}, {6273, "Google"}, {6274, "Bing"}, {6275, "Yahoo!"}, {6276, "Set your search engine here:"}},
             [&st](int v) { st.searchEngine = v; }, {}, true);
    b.entry(0, "", st.searchEngineCustom, 60, 1);
    b.note(b.text(6278, "Example: https://www.google.com/search?q=$(CURRENT_WORD)"));

    // ---- MISC. -------------------------------------------------------------------------
    b.page(stack, "MISC", "MISC.");
    b.heading(6312, "File Status Auto-Detection");
    const auto detection = comboItems(tr_, "MISC", 6347, {"Enable for current file", "Enable for all opened files", "Disable"});
    int det = st.changeDetection == platform::ChangeDetection::Off ? 1 : 0;
    b.radios(det, {{0, detection[1].c_str()}, {0, detection[2].c_str()}}, [&st](int v) {
        if (v == 1)
            st.changeDetection = platform::ChangeDetection::Off;
        else if (st.changeDetection == platform::ChangeDetection::Off)
            st.changeDetection = platform::ChangeDetection::Notify;
    });
    bool silent = st.changeDetection == platform::ChangeDetection::AutoUpdate;
    GtkWidget* silentBox = b.check(6313, "Update silently", silent, 1);
    b.bind(silentBox, "toggled", [&st, silentBox] {
        if (st.changeDetection != platform::ChangeDetection::Off)
            st.changeDetection = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(silentBox)) ? platform::ChangeDetection::AutoUpdate
                                                                                           : platform::ChangeDetection::Notify;
    });
    b.check(6325, "Scroll to the last line after update", st.scrollToEndAfterUpdate, 1);
    b.heading(6324, "Document Switcher (Ctrl+TAB)");
    GtkWidget* sw = b.check(6114, "Enable", st.docSwitcher);
    GtkWidget* mru = b.check(6117, "Enable MRU behaviour", st.docSwitcherMru, 1);
    b.enables(sw, {mru});
    b.heading(0, "");
    b.check(6331, "Show only filename in title bar", st.titleFileNameOnly);
    b.check(6334, "Autodetect character encoding", st.autodetectEncoding);
    b.check(6360, "Mute all sounds", st.muteSounds);
    b.check(6361, "Enable Save All confirm dialog", st.saveAllConfirm);
    b.check(6366, "Allow loading symlinks in Folder as Workspace panel", st.faWSymlinks);
    b.entry(6322, "Session file ext.:", st.sessionExt, 8);
    b.entry(6337, "Workspace file ext.:", st.workspaceExt, 8);

    gtk_widget_show_all(content);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    settings_.save();
}

} // namespace npp::ui

// ============================================================================
// Applying the preferences
// ============================================================================

namespace npp::ui {

namespace {

// Windows date/time picture ("yyyy-MM-dd HH:mm:ss", text in '...'), as upstream
// passes it to GetDateFormat/GetTimeFormat.
std::string windowsDateTime(const std::string& fmt, GDateTime* t)
{
    std::string out;
    auto run = [&](std::size_t i) {
        std::size_t n = 1;
        while (i + n < fmt.size() && fmt[i + n] == fmt[i])
            ++n;
        return n;
    };
    auto two = [](int v) {
        char b[8];
        std::snprintf(b, sizeof b, "%02d", v);
        return std::string(b);
    };
    auto glibFmt = [t](const char* f) {
        gchar* s = g_date_time_format(t, f);
        std::string r = s ? s : "";
        g_free(s);
        return r;
    };
    for (std::size_t i = 0; i < fmt.size();) {
        const char c = fmt[i];
        if (c == '\'') { // quoted text; '' is a quote
            std::size_t j = i + 1;
            if (j < fmt.size() && fmt[j] == '\'') {
                out += '\'';
                i = j + 1;
                continue;
            }
            for (; j < fmt.size(); ++j) {
                if (fmt[j] != '\'')
                    out += fmt[j];
                else if (j + 1 < fmt.size() && fmt[j + 1] == '\'')
                    out += fmt[j++];
                else
                    break;
            }
            i = j + 1;
            continue;
        }
        const std::size_t n = run(i);
        switch (c) {
        case 'y': out += n >= 3 ? std::to_string(g_date_time_get_year(t)) : two(g_date_time_get_year(t) % 100); break;
        case 'M': out += n >= 4 ? glibFmt("%B") : n == 3 ? glibFmt("%b") : n == 2 ? two(g_date_time_get_month(t)) : std::to_string(g_date_time_get_month(t)); break;
        case 'd': out += n >= 4 ? glibFmt("%A") : n == 3 ? glibFmt("%a") : n == 2 ? two(g_date_time_get_day_of_month(t)) : std::to_string(g_date_time_get_day_of_month(t)); break;
        case 'H': out += n >= 2 ? two(g_date_time_get_hour(t)) : std::to_string(g_date_time_get_hour(t)); break;
        case 'h': {
            const int h = g_date_time_get_hour(t) % 12 == 0 ? 12 : g_date_time_get_hour(t) % 12;
            out += n >= 2 ? two(h) : std::to_string(h);
            break;
        }
        case 'm': out += n >= 2 ? two(g_date_time_get_minute(t)) : std::to_string(g_date_time_get_minute(t)); break;
        case 's': out += n >= 2 ? two(g_date_time_get_second(t)) : std::to_string(g_date_time_get_second(t)); break;
        case 't': out += (g_date_time_get_hour(t) < 12 ? std::string("AM") : std::string("PM")).substr(0, n >= 2 ? 2 : 1); break;
        default: out.append(n, c); break;
        }
        i += n;
    }
    return out;
}

} // namespace

std::string MainWindow::formatCustomDate(const std::string& windowsFormat)
{
    GDateTime* now = g_date_time_new_now_local();
    std::string s = windowsDateTime(windowsFormat, now);
    g_date_time_unref(now);
    return s;
}

fs::path MainWindow::dialogFolder() const
{
    // Preferences > Default Directory.
    if (settings_.defaultDirMode == 2 && !settings_.defaultDir.empty())
        return settings_.defaultDir;
    if (settings_.defaultDirMode == 0)
        if (const core::Buffer* b = activeBuffer(); b && !b->isUntitled())
            return b->path.parent_path();
    return lastDir_;
}

bool MainWindow::desktopPrefersDark() const
{
    // The desktop's colour scheme through the Settings portal
    // (org.freedesktop.appearance color-scheme: 1 = prefer dark), else the
    // GTK theme's name.
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus) {
        GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                  "org.freedesktop.portal.Settings", "ReadOne",
                                                  g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"), nullptr,
                                                  G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
        g_object_unref(bus);
        if (r) {
            GVariant* v = nullptr;
            g_variant_get(r, "(v)", &v);
            guint32 scheme = 0;
            if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32))
                scheme = g_variant_get_uint32(v);
            if (v)
                g_variant_unref(v);
            g_variant_unref(r);
            return scheme == 1;
        }
    }
    gchar* name = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, nullptr);
    const bool dark = name && strcasestr(name, "dark");
    g_free(name);
    return dark;
}

void MainWindow::applyDarkMode()
{
    platform::Settings& st = settings_;
    const bool dark = st.darkMode == 1 || (st.darkMode == 2 && desktopPrefersDark());
    if (dark && st.theme != st.darkTheme) {
        st.lightTheme = st.theme;
        cmdSetTheme(st.darkTheme);
    } else if (!dark && st.theme == st.darkTheme && st.darkMode != 1) {
        cmdSetTheme(st.lightTheme == st.darkTheme ? std::string() : st.lightTheme);
    }
    refreshThemeMenu();
    syncActionStates();
}

void MainWindow::updateEditorOptions()
{
    const platform::Settings& st = settings_;
    EditorOptions& o = editorOpts_;
    o.autoIndentMode = st.autoIndentMode;
    o.smartHighlight = st.smartHighlight;
    o.smartWholeWord = st.smartUseFind ? st.find.wholeWord : st.smartWholeWord;
    o.smartMatchCase = st.smartUseFind ? st.find.matchCase : st.smartMatchCase;
    o.smartOtherView = st.smartOtherView;
    o.tagMatch = st.tagMatch;
    o.tagAttributes = st.tagAttributes;
    o.tagZones = st.tagZones;
    o.links = st.linkEnable;
    o.linkSchemes = st.linkSchemes;
    o.delimOpen = st.delimOpen.empty() ? '\0' : st.delimOpen[0];
    o.delimClose = st.delimClose.empty() ? '\0' : st.delimClose[0];
    o.delimMultiLine = st.delimMultiLine;
    o.preventC0 = st.preventC0;
    o.keepSelOnRightClick = st.keepSelOnRightClick;
    o.lineNumberDynamic = st.lineNumberDynamic;
    o.pairs.parentheses = st.insertParen;
    o.pairs.brackets = st.insertBracket;
    o.pairs.braces = st.insertBrace;
    o.pairs.doubleQuotes = st.insertDQuote;
    o.pairs.quotes = st.insertSQuote;
    o.pairs.htmlXmlTag = st.insertCloseTag;
    o.pairs.user.clear();
    for (std::size_t i = 0; i < 3; ++i)
        if (!st.pairOpen[i].empty() && !st.pairClose[i].empty())
            o.pairs.user.emplace_back(st.pairOpen[i][0], st.pairClose[i][0]);
    o.autoComplete = {st.acEnable, st.acMode, st.acFromChar, st.acIgnoreNumbers, st.acBrief, st.acParamHint};
    o.acInsertTab = st.acInsertTab;
    o.acInsertEnter = st.acInsertEnter;
    o.largeBraceMatch = st.largeAllowBrace;
    o.largeAutoComplete = st.largeAllowAC;
    o.largeSmartHighlight = st.largeAllowSmart;
    o.largeLinks = st.largeAllowLinks;
}

const core::ApiFile* MainWindow::apiFor(const core::Buffer& b)
{
    if (!apis_)
        apis_ = std::make_unique<core::ApiCatalog>(platform::dataDir() / "autoCompletion");
    if (b.userLanguage)
        return apis_->forLanguage(b.userLanguage->name);
    if (!b.language || b.language->isPlainText())
        return nullptr;
    return apis_->forLanguage(b.language->name);
}

void MainWindow::applyToolbarIcons()
{
    // Preferences > Toolbar: upstream's icon sets, drawn for the screen's
    // scale. Its dark mode icons go on a dark toolbar: the GTK theme's, which
    // stays light with a dark editor theme when it has no dark variant.
    GdkRGBA fg;
    gtk_style_context_get_color(gtk_widget_get_style_context(toolbar_), GTK_STATE_FLAG_NORMAL, &fg);
    const bool dark = 0.2126 * fg.red + 0.7152 * fg.green + 0.0722 * fg.blue > 0.5; // light text
    const int set = settings_.toolbarIconSet;
    const int scale = gtk_widget_get_scale_factor(toolbar_);
    const int px = toolbarIconPixels(set);
    const fs::path dir = platform::dataDir() / "icons";
    auto surface = [&](const fs::path& file) -> cairo_surface_t* {
        if (file.empty())
            return nullptr;
        GdkPixbuf* pb = loadToolbarIcon(file, px * scale);
        if (!pb)
            return nullptr;
        cairo_surface_t* s = gdk_cairo_surface_create_from_pixbuf(pb, scale, nullptr);
        g_object_unref(pb);
        return s;
    };
    const int n = gtk_toolbar_get_n_items(GTK_TOOLBAR(toolbar_));
    for (int i = 0; i < n; ++i) {
        GtkToolItem* it = gtk_toolbar_get_nth_item(GTK_TOOLBAR(toolbar_), i);
        const auto* b = static_cast<const ToolbarButton*>(g_object_get_data(G_OBJECT(it), "npp-button"));
        if (!b)
            continue; // a separator or a plugin's button
        g_object_set_data_full(G_OBJECT(it), "npp-icon", surface(toolbarIconFile(dir, *b, set, dark, false)),
                               reinterpret_cast<GDestroyNotify>(cairo_surface_destroy));
        g_object_set_data_full(G_OBJECT(it), "npp-icon-dis", surface(toolbarIconFile(dir, *b, set, dark, true)),
                               reinterpret_cast<GDestroyNotify>(cairo_surface_destroy));
        g_object_set_data(G_OBJECT(it), "npp-icon-px", GINT_TO_POINTER(px));
        showToolbarIcon(GTK_WIDGET(it));
    }
}

void MainWindow::showToolbarIcon(GtkWidget* item)
{
    const auto* b = static_cast<const ToolbarButton*>(g_object_get_data(G_OBJECT(item), "npp-button"));
    GtkWidget* image = b ? gtk_tool_button_get_icon_widget(GTK_TOOL_BUTTON(item)) : nullptr;
    if (!image)
        return;
    auto* normal = static_cast<cairo_surface_t*>(g_object_get_data(G_OBJECT(item), "npp-icon"));
    auto* disabled = static_cast<cairo_surface_t*>(g_object_get_data(G_OBJECT(item), "npp-icon-dis"));
    GtkWidget* button = gtk_bin_get_child(GTK_BIN(item));
    cairo_surface_t* s = disabled && button && !gtk_widget_is_sensitive(button) ? disabled : normal;
    if (s) {
        if (gtk_image_get_storage_type(GTK_IMAGE(image)) != GTK_IMAGE_SURFACE || g_object_get_data(G_OBJECT(image), "npp-shown") != s) {
            gtk_image_set_from_surface(GTK_IMAGE(image), s);
            g_object_set_data(G_OBJECT(image), "npp-shown", s);
        }
        return;
    }
    // Icon files missing: the desktop theme's icon.
    const int px = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "npp-icon-px"));
    gtk_image_set_from_icon_name(GTK_IMAGE(image), b->theme, px > 16 ? GTK_ICON_SIZE_LARGE_TOOLBAR : GTK_ICON_SIZE_SMALL_TOOLBAR);
    g_object_set_data(G_OBJECT(image), "npp-shown", nullptr);
}

void MainWindow::applyTabBar()
{
    const platform::Settings& st = settings_;
    // Colours from the theme, as upstream: "Active tab focused indicator",
    // "Inactive tabs".
    auto hex = [](std::uint32_t bgr) {
        char b[8];
        std::snprintf(b, sizeof b, "#%02x%02x%02x", bgr & 0xFF, (bgr >> 8) & 0xFF, (bgr >> 16) & 0xFF);
        return std::string(b);
    };
    std::string css;
    if (st.tabReduce)
        css += "notebook.npp-docs tab { padding: 1px 4px; min-height: 0; } notebook.npp-docs tab label { font-size: smaller; }\n";
    if (st.tabActiveBar) {
        const core::Style* s = styles_.global("Active tab focused indicator");
        css += "notebook.npp-docs tab:checked { box-shadow: inset 0 3px " + hex(s && s->fg ? *s->fg : 0x3CAAFA) + "; }\n";
    }
    if (st.tabInactiveColor) {
        const core::Style* s = styles_.global("Inactive tabs");
        if (s && s->fg)
            css += "notebook.npp-docs tab:not(:checked) label { color: " + hex(*s->fg) + "; }\n";
    }
    if (!tabCss_) {
        tabCss_ = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(tabCss_), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(tabCss_, css.c_str(), -1, nullptr);

    for (GtkWidget* nb : notebooks_) {
        gtk_style_context_add_class(gtk_widget_get_style_context(nb), "npp-docs");
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nb), !st.tabHide && !tabBarHidden_);
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(nb), st.tabVertical ? GTK_POS_LEFT : GTK_POS_TOP);
    }
    for (const auto& t : tabs_) {
        gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebooks_[t->viewIdx]), t->view->widget(), !st.tabLock);
        refreshTab(*t);
    }
}

void MainWindow::applySettings()
{
    updateEditorOptions();
    for (auto& t : tabs_) {
        applyViewSettings(*t->view);
        t->view->refreshDecorations();
    }
    if (menubar_)
        gtk_widget_set_visible(menubar_, !settings_.hideMenu && !postIt_);
    if (toolbar_) {
        gtk_widget_set_visible(toolbar_, !settings_.hideToolbar && !postIt_);
        applyToolbarIcons();
    }
    if (statusBar_)
        gtk_widget_set_visible(statusBar_, !settings_.hideStatusBar && !postIt_);
    applyTabBar();
    g_object_set(gtk_settings_get_default(), "gtk-error-bell", !settings_.muteSounds, nullptr);
    if (findDlg_) {
        const platform::Settings& st = settings_;
        findDlg_->setPrefs({st.fillFindWithSelection, st.fillFindMaxChars, st.selectWordUnderCaret, st.inSelectionThreshold, st.fifDirFromDoc,
                            st.findMonospace, st.findStaysOpen, st.confirmReplaceAllDocs, st.replaceNoMove});
    }
    if (results_)
        results_->oneEntryPerLine = settings_.oneEntryPerLine;
    recent_.setMax(static_cast<std::size_t>(std::max(settings_.maxRecentFiles, 0)));
    refreshRecentMenu();
    refreshLanguageMenu();
    refreshTitle();
    rewatchAll();
    restartSnapshotTimer();
    syncActionStates();
}

} // namespace npp::ui
