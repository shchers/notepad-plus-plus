// Menu commands keyed by upstream command IDs — the GTK side of upstream
// NppCommands.cpp. Commands that need a dialog of their own register it
// here too, so the menus show exactly what the port implements.

#include "ui/MainWindow.h"

#include "core/EditOps.h"
#include "core/NppConfig.h"
#include "core/SearchEngine.h"
#include "platform/Paths.h"
#include "platform/Sandbox.h"

#include "menuCmdID.h"
#include <windows.h> // plugin SDK: NPPN_* codes
#include "Notepad_plus_msgs.h"

#include <algorithm>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::ui {

namespace {

constexpr int kBookmarkMarker = 24; // upstream MARK_BOOKMARK, see EditorView

void setClipboard(const std::string& text)
{
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text.c_str(), static_cast<gint>(text.size()));
}

std::string clipboardText()
{
    gchar* t = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    std::string s = t ? t : "";
    g_free(t);
    return s;
}

// Upstream EncodingMapper order: IDM_FORMAT_ENCODE + index -> code page.
constexpr int kEncodeCodepages[] = {
    1250, 1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258,                     // Windows
    28591, 28592, 28593, 28594, 28595, 28596, 28597, 28598, 28599, -1, -1,     // ISO 8859-1..11
    28603, 28604, 28605, -1,                                                    // ISO 8859-13..16
    437, 720, 737, 775, 850, 852, 855, 857, 858, 860, 861, 862, 863, 865, 866, 869, // OEM
    950, 936, 932, 949, 51949, 874, 10007, 21866, 20866,
};
static_assert(sizeof(kEncodeCodepages) / sizeof(int) == IDM_FORMAT_ENCODE_END - IDM_FORMAT_ENCODE + 1);

// Runs a native file chooser; returns the chosen path or empty.
fs::path chooseFile(GtkWindow* parent, const std::string& title, GtkFileChooserAction action, const char* accept,
                    const fs::path& folder, const char* pattern, const char* patternName)
{
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(title.c_str(), parent, action, accept, "_Cancel");
    if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (!folder.empty())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), folder.c_str());
    if (pattern) {
        GtkFileFilter* f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, patternName);
        gtk_file_filter_add_pattern(f, pattern);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
        GtkFileFilter* all = gtk_file_filter_new();
        gtk_file_filter_set_name(all, "All files");
        gtk_file_filter_add_pattern(all, "*");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    }
    fs::path out;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        if (gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg))) {
            out = fn;
            g_free(fn);
        }
    g_object_unref(dlg);
    return out;
}

// A one-line text prompt; returns false when cancelled.
bool promptText(GtkWindow* parent, const std::string& title, const std::string& label, std::string& value)
{
    GtkWidget* dlg = gtk_dialog_new_with_buttons(title.c_str(), parent, GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget* l = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 4);
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), value.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 40);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(box);
    const bool ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
    if (ok)
        value = gtk_entry_get_text(GTK_ENTRY(entry));
    gtk_widget_destroy(dlg);
    return ok;
}

std::string formatNow(const char* fmt)
{
    GDateTime* now = g_date_time_new_now_local();
    gchar* s = g_date_time_format(now, fmt);
    std::string out = s ? s : "";
    g_free(s);
    g_date_time_unref(now);
    return out;
}

std::string checksum(GChecksumType type, const std::string& data)
{
    gchar* hex = g_compute_checksum_for_data(type, reinterpret_cast<const guchar*>(data.data()), data.size());
    std::string out = hex ? hex : "";
    g_free(hex);
    return out;
}

} // namespace

// ============================================================================
// Helpers
// ============================================================================

void MainWindow::withSci(const std::function<void(const core::Sci&)>& fn)
{
    if (EditorView* v = activeView())
        fn(v->sci());
}

void MainWindow::editOp(const std::function<void(const core::Sci&)>& fn)
{
    EditorView* v = activeView();
    if (!v || v->execute(SCI_GETREADONLY))
        return;
    fn(v->sci());
}

void MainWindow::sciCommand(unsigned msg)
{
    if (EditorView* v = activeView())
        v->execute(msg);
}

const char* MainWindow::lineCommentFor(const core::Buffer& b) const
{
    return b.language ? b.language->commentLine.c_str() : "";
}

bool MainWindow::currentEncodingIs(const platform::TextEncoding& e) const
{
    const core::Buffer* b = activeBuffer();
    if (!b || b->encoding.bom != e.bom)
        return false;
    if (e.bom != platform::Bom::None)
        return true;
    return platform::codepageForCharset(b->encoding.charset) == platform::codepageForCharset(e.charset)
           && (platform::codepageForCharset(e.charset) != -1 || g_ascii_strcasecmp(b->encoding.charset.c_str(), e.charset.c_str()) == 0);
}

void MainWindow::openUri(const std::string& uri)
{
    GError* err = nullptr;
    if (!gtk_show_uri_on_window(GTK_WINDOW(window_), uri.c_str(), GDK_CURRENT_TIME, &err)) {
        showError("Cannot open " + uri, err ? err->message : "");
        if (err)
            g_error_free(err);
    }
}

void MainWindow::openTerminal(const fs::path& dir)
{
    switch (platform::currentSandbox()) {
    case platform::Sandbox::Flatpak: { // the host's terminal, through flatpak-spawn
        const std::vector<std::string> args = platform::hostCommand(platform::terminalCommand(), dir, platform::Sandbox::Flatpak);
        std::vector<gchar*> argv;
        for (const std::string& a : args)
            argv.push_back(const_cast<gchar*>(a.c_str()));
        argv.push_back(nullptr);
        GError* err = nullptr;
        if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &err)) {
            showError("Cannot open a terminal", err ? err->message : "");
            if (err)
                g_error_free(err);
        }
        return;
    }
    case platform::Sandbox::Snap:
        showError("Cannot open a terminal", "A terminal cannot be started from the Snap sandbox.");
        return;
    case platform::Sandbox::None:
        break;
    }
    // The desktop's terminal: Debian's alternative, then common ones.
    for (const char* t : {"x-terminal-emulator", "gnome-terminal", "konsole", "xfce4-terminal", "mate-terminal", "xterm"}) {
        gchar* prog = g_find_program_in_path(t);
        if (!prog)
            continue;
        gchar* argv[] = {prog, nullptr};
        GError* err = nullptr;
        const gboolean ok = g_spawn_async(dir.c_str(), argv, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &err);
        g_free(prog);
        if (ok)
            return;
        if (err)
            g_error_free(err);
    }
    showError("Cannot open a terminal", "No terminal emulator was found.");
}

MainWindow::Tab* MainWindow::tabAt(int viewIdx, int page) const
{
    GtkWidget* w = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebooks_[viewIdx]), page);
    return w ? tabForPage(w) : nullptr;
}

void MainWindow::moveTab(int to)
{
    Tab* t = activeTab();
    if (!t)
        return;
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[t->viewIdx]);
    const int n = gtk_notebook_get_n_pages(nb);
    const int index = to < 0 ? n - 1 : std::clamp(to, 0, n - 1);
    gtk_notebook_reorder_child(nb, t->view->widget(), index);
    // Upstream: hwndFrom is the new index.
    notifyPlugins(NPPN_DOCORDERCHANGED, reinterpret_cast<const void*>(static_cast<intptr_t>(index)), reinterpret_cast<uptr_t>(t->buf.get()));
}

// ============================================================================
// Registration
// ============================================================================

void MainWindow::registerCommands()
{
    registerFileCommands();
    registerEditCommands();
    registerSearchCommands();
    registerViewCommands();
    registerFormatCommands();
    registerOtherCommands();
    registerMacroCommands();
    registerPanelCommands();
}

void MainWindow::registerFileCommands()
{
    auto buf = [this] { return activeBuffer(); };
    auto onDisk = [this]() -> core::Buffer* {
        core::Buffer* b = activeBuffer();
        return b && !b->isUntitled() ? b : nullptr;
    };
    cmds_.add(IDM_FILE_NEW, [this] { newDocument(); });
    cmds_.add(IDM_FILE_OPEN, [this] { cmdOpen(); });
    cmds_.add(IDM_FILE_OPEN_FOLDER, [this, onDisk] {
        if (core::Buffer* b = onDisk()) {
            gchar* uri = g_filename_to_uri(b->path.parent_path().c_str(), nullptr, nullptr);
            if (uri)
                openUri(uri);
            g_free(uri);
        }
    });
    cmds_.add(IDM_FILE_OPEN_CMD, [this, onDisk] {
        if (core::Buffer* b = onDisk())
            openTerminal(b->path.parent_path());
    });
    cmds_.add(IDM_FILE_OPEN_DEFAULT_VIEWER, [this, onDisk] {
        if (core::Buffer* b = onDisk()) {
            gchar* uri = g_filename_to_uri(b->path.c_str(), nullptr, nullptr);
            if (uri)
                openUri(uri);
            g_free(uri);
        }
    });
    cmds_.add(IDM_FILE_RELOAD, [this, onDisk] {
        core::Buffer* b = onDisk();
        if (!b)
            return;
        if (isModified(*b)
            && ask(trMessage("DocReloadWarning", "message",
                             "Are you sure you want to reload the current file and lose the changes made in Notepad++?"),
                   b->path.string(), false) != Answer::Yes)
            return;
        std::string err;
        if (!reloadBuffer(*b, &err))
            showError("Cannot reload file", err);
    });
    cmds_.add(IDM_FILE_SAVE, [this, buf] { if (core::Buffer* b = buf()) save(*b, false); });
    cmds_.add(IDM_FILE_SAVEAS, [this, buf] { if (core::Buffer* b = buf()) save(*b, true); });
    cmds_.add(IDM_FILE_SAVECOPYAS, [this, buf] { if (core::Buffer* b = buf()) saveCopyAs(*b); });
    cmds_.add(IDM_FILE_SAVEALL, [this] {
        // Preferences > MISC.: "Enable Save All confirm dialog".
        if (settings_.saveAllConfirm && ask(trMessage("SaveAllConfirm", "message", "Are you sure you want to save all modified documents?"),
                                            "", false) != Answer::Yes)
            return;
        for (core::Buffer* b : buffers())
            if (isModified(*b) && !save(*b, false))
                break;
    });
    cmds_.add(IDM_FILE_RENAME, [this, buf] { if (core::Buffer* b = buf()) renameBuffer(*b); });
    cmds_.add(IDM_FILE_CLOSE, [this] { if (Tab* t = activeTab()) closeTab(*t); });
    cmds_.add(IDM_FILE_CLOSEALL, [this] { closeAll(); });
    cmds_.add(IDM_FILE_CLOSEALL_BUT_CURRENT, [this] {
        Tab* keep = activeTab();
        if (!keep)
            return;
        for (int vi : {kSubView, kMainView})
            for (Tab* t : tabsInOrder(vi))
                if (t->buf != keep->buf && !closeTab(*t, false))
                    return;
        for (Tab* t : tabsOf(*keep->buf)) // its clone goes too
            if (t != keep)
                removeTab(*t);
        activateTab(*keep);
    });
    cmds_.add(IDM_FILE_CLOSEALL_TOLEFT, [this] { closeTabsWhere([](Tab&, int i, int active) { return i < active; }); });
    cmds_.add(IDM_FILE_CLOSEALL_TORIGHT, [this] { closeTabsWhere([](Tab&, int i, int active) { return i > active; }); });
    cmds_.add(IDM_FILE_CLOSEALL_UNCHANGED, [this] {
        closeTabsWhere([this](Tab& t, int, int) { return !isModified(*t.buf); });
    });
    cmds_.add(IDM_FILE_DELETE, [this, onDisk] {
        core::Buffer* b = onDisk();
        if (!b)
            return;
        if (ask(trMessage("DeleteFileWarning", "message", "Are you sure you want to move the current file to the Recycle Bin?"),
                b->path.string(), false) != Answer::Yes)
            return;
        notifyPlugins(NPPN_FILEBEFOREDELETE, b);
        GFile* f = g_file_new_for_path(b->path.c_str());
        GError* err = nullptr;
        if (!g_file_trash(f, nullptr, &err)) {
            notifyPlugins(NPPN_FILEDELETEFAILED, b);
            showError("Cannot move the file to the trash", err ? err->message : b->path.string());
            if (err)
                g_error_free(err);
            g_object_unref(f);
            return;
        }
        g_object_unref(f);
        monitors_.erase(b);
        const std::shared_ptr<core::Buffer> keep = tabsOf(*b).front()->buf; // the ID stays valid for NPPN_FILEDELETED
        for (Tab* t : tabsOf(*b))
            removeTab(*t);
        notifyPlugins(NPPN_FILEDELETED, keep.get());
        if (tabs_.empty())
            newDocument();
        refreshAll();
    });
    cmds_.add(IDM_FILE_LOADSESSION, [this] { cmdLoadSession(); });
    cmds_.add(IDM_SETTING_PLUGINADM, [this] { cmdPluginsAdmin(); });
    cmds_.add(IDM_SETTING_IMPORTPLUGIN, [this] { cmdImportPlugins(); });
    // Plugins > Open Plugins Folder: where the user installs plugins.
    cmds_.add(IDM_SETTING_OPENPLUGINSDIR, [this] {
        const auto dirs = PluginManager::defaultDirs();
        if (dirs.empty())
            return;
        std::error_code ec;
        fs::create_directories(dirs.front(), ec);
        gchar* uri = g_filename_to_uri(dirs.front().c_str(), nullptr, nullptr);
        if (uri)
            openUri(uri);
        g_free(uri);
    });
    cmds_.add(IDM_FILE_SAVESESSION, [this] { cmdSaveSession(); });
    cmds_.add(IDM_FILE_RESTORELASTCLOSEDFILE, [this] { cmdRestoreRecent(); });
    cmds_.add(IDM_OPEN_ALL_RECENT_FILE, [this] { cmdOpenAllRecent(); });
    cmds_.add(IDM_CLEAN_RECENT_FILE_LIST, [this] { recent_.clear(); refreshRecentMenu(); });
    cmds_.add(IDM_FILE_EXIT, [this] { gtk_window_close(GTK_WINDOW(window_)); });
}

void MainWindow::registerEditCommands()
{
    auto buf = [this] { return activeBuffer(); };
    cmds_.add(IDM_EDIT_UNDO, [this] { sciCommand(SCI_UNDO); });
    cmds_.add(IDM_EDIT_REDO, [this] { sciCommand(SCI_REDO); });
    // Preferences > Editing 1: "Copy/Cut Line without selection", as upstream.
    cmds_.add(IDM_EDIT_CUT, [this] {
        EditorView* v = activeView();
        if (v && v->execute(SCI_GETSELECTIONEMPTY) && settings_.copyLineNoSelection) {
            if (!v->execute(SCI_GETREADONLY)) {
                v->execute(SCI_COPYALLOWLINE);
                v->execute(SCI_LINEDELETE);
            }
        } else if (!v || !v->execute(SCI_GETSELECTIONEMPTY)) {
            sciCommand(SCI_CUT);
        }
    });
    cmds_.add(IDM_EDIT_COPY, [this] {
        EditorView* v = activeView();
        if (v && v->execute(SCI_GETSELECTIONEMPTY) && settings_.copyLineNoSelection)
            v->execute(SCI_COPYALLOWLINE);
        else
            sciCommand(SCI_COPY);
    });
    cmds_.add(IDM_EDIT_PASTE, [this] { sciCommand(SCI_PASTE); });
    cmds_.add(IDM_EDIT_DELETE, [this] { sciCommand(SCI_CLEAR); });
    cmds_.add(IDM_EDIT_SELECTALL, [this] { sciCommand(SCI_SELECTALL); });
    cmds_.add(IDM_EDIT_BEGINENDSELECT, [this] { cmdBeginEndSelect(false); }, [this] { return beginSelectPos_ >= 0; });
    cmds_.add(IDM_EDIT_BEGINENDSELECT_COLUMNMODE, [this] { cmdBeginEndSelect(true); }, [this] { return beginSelectPos_ >= 0; });
    // Upstream: time then date, or date then time with "Reverse default date time order".
    auto insertDateTime = [this](const char* date) {
        const std::string time = formatNow("%H:%M"), d = formatNow(date);
        const std::string s = settings_.dateReverse ? d + " " + time : time + " " + d;
        editOp([&](const core::Sci& sci) { sci(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(s.c_str())); });
    };
    cmds_.add(IDM_EDIT_INSERT_DATETIME_SHORT, [insertDateTime] { insertDateTime("%x"); });
    cmds_.add(IDM_EDIT_INSERT_DATETIME_LONG, [insertDateTime] { insertDateTime("%A, %B %e, %Y"); });
    cmds_.add(IDM_EDIT_INSERT_DATETIME_CUSTOMIZED, [this] {
        const std::string s = formatCustomDate(settings_.dateFormat);
        editOp([&](const core::Sci& sci) { sci(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(s.c_str())); });
    });
    // Edit > Auto-Completion (upstream AutoCompletion / FunctionCallTip).
    auto completion = [this](int what) {
        Tab* t = activeTab();
        if (!t)
            return;
        const core::ApiFile* api = apiFor(*t->buf);
        core::AutoCompleter& c = t->view->completer();
        const core::Sci sci = t->view->sci();
        switch (what) {
        case 0: c.showFunctionList(sci, api); break;
        case 1: c.showWordList(sci, api, true, settings_.acIgnoreNumbers); break;
        case 2: c.showCallTip(sci, api); break;
        case 3: c.callTipClick(sci, 1); break;
        case 4: c.callTipClick(sci, 2); break;
        default: break;
        }
    };
    cmds_.add(IDM_EDIT_AUTOCOMPLETE, [completion] { completion(0); });
    cmds_.add(IDM_EDIT_AUTOCOMPLETE_CURRENTFILE, [completion] { completion(1); });
    cmds_.add(IDM_EDIT_FUNCCALLTIP, [completion] { completion(2); });
    cmds_.add(IDM_EDIT_FUNCCALLTIP_PREVIOUS, [completion] { completion(3); });
    cmds_.add(IDM_EDIT_FUNCCALLTIP_NEXT, [completion] { completion(4); });

    cmds_.add(IDM_EDIT_FULLPATHTOCLIP, [buf] { if (core::Buffer* b = buf()) setClipboard(b->fullName()); });
    cmds_.add(IDM_EDIT_FILENAMETOCLIP, [buf] { if (core::Buffer* b = buf()) setClipboard(b->name()); });
    cmds_.add(IDM_EDIT_CURRENTDIRTOCLIP, [buf] {
        if (core::Buffer* b = buf(); b && !b->isUntitled())
            setClipboard(b->path.parent_path().string());
    });
    auto allNames = [this](bool full) {
        std::string out;
        for (int vi : {kMainView, kSubView})
            for (Tab* t : tabsInOrder(vi)) {
                if (vi == kSubView && tabOf(*t->buf, kMainView))
                    continue; // a clone is listed once
                out += (full ? t->buf->fullName() : t->buf->name()) + "\n";
            }
        setClipboard(out);
    };
    cmds_.add(IDM_EDIT_COPY_ALL_NAMES, [allNames] { allNames(false); });
    cmds_.add(IDM_EDIT_COPY_ALL_PATHS, [allNames] { allNames(true); });

    cmds_.add(IDM_EDIT_INS_TAB, [this] { editOp([](const core::Sci& s) { s(SCI_TAB); }); });
    cmds_.add(IDM_EDIT_RMV_TAB, [this] { editOp([](const core::Sci& s) { s(SCI_BACKTAB); }); });

    using core::TextCase;
    const std::pair<int, TextCase> cases[] = {
        {IDM_EDIT_UPPERCASE, TextCase::Upper}, {IDM_EDIT_LOWERCASE, TextCase::Lower},
        {IDM_EDIT_PROPERCASE_FORCE, TextCase::ProperForce}, {IDM_EDIT_PROPERCASE_BLEND, TextCase::ProperBlend},
        {IDM_EDIT_SENTENCECASE_FORCE, TextCase::SentenceForce}, {IDM_EDIT_SENTENCECASE_BLEND, TextCase::SentenceBlend},
        {IDM_EDIT_INVERTCASE, TextCase::Invert}, {IDM_EDIT_RANDOMCASE, TextCase::Random},
    };
    for (const auto& [id, c] : cases)
        cmds_.add(id, [this, c = c] { editOp([c](const core::Sci& s) { core::convertSelectionCase(s, c); }); });

    cmds_.add(IDM_EDIT_DUP_LINE, [this] { editOp([](const core::Sci& s) { s(SCI_LINEDUPLICATE); }); });
    cmds_.add(IDM_EDIT_REMOVE_ANY_DUP_LINES, [this] { editOp([](const core::Sci& s) { core::removeDuplicateLines(s, false); }); });
    cmds_.add(IDM_EDIT_REMOVE_CONSECUTIVE_DUP_LINES, [this] { editOp([](const core::Sci& s) { core::removeDuplicateLines(s, true); }); });
    cmds_.add(IDM_EDIT_SPLIT_LINES, [this] { editOp([](const core::Sci& s) { core::splitLines(s); }); });
    cmds_.add(IDM_EDIT_JOIN_LINES, [this] { editOp([](const core::Sci& s) { core::joinLines(s); }); });
    cmds_.add(IDM_EDIT_LINE_UP, [this] { editOp([](const core::Sci& s) { s(SCI_MOVESELECTEDLINESUP); }); });
    cmds_.add(IDM_EDIT_LINE_DOWN, [this] { editOp([](const core::Sci& s) { s(SCI_MOVESELECTEDLINESDOWN); }); });
    cmds_.add(IDM_EDIT_REMOVEEMPTYLINES, [this] { editOp([](const core::Sci& s) { core::removeEmptyLines(s, false); }); });
    cmds_.add(IDM_EDIT_REMOVEEMPTYLINESWITHBLANK, [this] { editOp([](const core::Sci& s) { core::removeEmptyLines(s, true); }); });
    cmds_.add(IDM_EDIT_BLANKLINEABOVECURRENT, [this] { editOp([](const core::Sci& s) { core::insertBlankLine(s, true); }); });
    cmds_.add(IDM_EDIT_BLANKLINEBELOWCURRENT, [this] { editOp([](const core::Sci& s) { core::insertBlankLine(s, false); }); });
    for (int id : {IDM_EDIT_SORTLINES_LEXICOGRAPHIC_ASCENDING, IDM_EDIT_SORTLINES_LEXICOGRAPHIC_DESCENDING,
                   IDM_EDIT_SORTLINES_LEXICO_CASE_INSENS_ASCENDING, IDM_EDIT_SORTLINES_LEXICO_CASE_INSENS_DESCENDING,
                   IDM_EDIT_SORTLINES_LOCALE_ASCENDING, IDM_EDIT_SORTLINES_LOCALE_DESCENDING,
                   IDM_EDIT_SORTLINES_INTEGER_ASCENDING, IDM_EDIT_SORTLINES_INTEGER_DESCENDING,
                   IDM_EDIT_SORTLINES_DECIMALCOMMA_ASCENDING, IDM_EDIT_SORTLINES_DECIMALCOMMA_DESCENDING,
                   IDM_EDIT_SORTLINES_DECIMALDOT_ASCENDING, IDM_EDIT_SORTLINES_DECIMALDOT_DESCENDING,
                   IDM_EDIT_SORTLINES_LENGTH_ASCENDING, IDM_EDIT_SORTLINES_LENGTH_DESCENDING,
                   IDM_EDIT_SORTLINES_REVERSE_ORDER, IDM_EDIT_SORTLINES_RANDOMLY})
        cmds_.add(id, [this, id] { cmdSort(id); });

    auto comment = [this](core::LineComment mode) {
        core::Buffer* b = activeBuffer();
        if (b)
            editOp([&](const core::Sci& s) { core::lineComment(s, lineCommentFor(*b), mode); });
    };
    cmds_.add(IDM_EDIT_BLOCK_COMMENT, [comment] { comment(core::LineComment::Toggle); });
    cmds_.add(IDM_EDIT_BLOCK_COMMENT_SET, [comment] { comment(core::LineComment::Add); });
    cmds_.add(IDM_EDIT_BLOCK_UNCOMMENT, [comment] { comment(core::LineComment::Remove); });
    auto stream = [this](bool add) {
        core::Buffer* b = activeBuffer();
        if (b && b->language)
            editOp([&](const core::Sci& s) { core::streamComment(s, b->language->commentStart, b->language->commentEnd, add); });
    };
    cmds_.add(IDM_EDIT_STREAM_COMMENT, [stream] { stream(true); });
    cmds_.add(IDM_EDIT_STREAM_UNCOMMENT, [stream] { stream(false); });

    auto eolChecked = [this](int mode) {
        EditorView* v = activeView();
        return v && v->execute(SCI_GETEOLMODE) == mode;
    };
    cmds_.add(IDM_FORMAT_TODOS, [this] { cmdSetEol(SC_EOL_CRLF); }, [eolChecked] { return eolChecked(SC_EOL_CRLF); });
    cmds_.add(IDM_FORMAT_TOUNIX, [this] { cmdSetEol(SC_EOL_LF); }, [eolChecked] { return eolChecked(SC_EOL_LF); });
    cmds_.add(IDM_FORMAT_TOMAC, [this] { cmdSetEol(SC_EOL_CR); }, [eolChecked] { return eolChecked(SC_EOL_CR); });

    cmds_.add(IDM_EDIT_TRIMTRAILING, [this] { editOp([](const core::Sci& s) { core::trim(s, core::Trim::Trailing); }); });
    cmds_.add(IDM_EDIT_TRIMLINEHEAD, [this] { editOp([](const core::Sci& s) { core::trim(s, core::Trim::Leading); }); });
    cmds_.add(IDM_EDIT_TRIM_BOTH, [this] { editOp([](const core::Sci& s) { core::trim(s, core::Trim::Both); }); });
    cmds_.add(IDM_EDIT_EOL2WS, [this] { editOp([](const core::Sci& s) { core::eolToSpace(s); }); });
    cmds_.add(IDM_EDIT_TRIMALL, [this] { editOp([](const core::Sci& s) { core::trimAll(s); }); });
    cmds_.add(IDM_EDIT_TAB2SW, [this] { editOp([](const core::Sci& s) { core::tabsToSpaces(s); }); });
    cmds_.add(IDM_EDIT_SW2TAB_ALL, [this] { editOp([](const core::Sci& s) { core::spacesToTabs(s, false); }); });
    cmds_.add(IDM_EDIT_SW2TAB_LEADING, [this] { editOp([](const core::Sci& s) { core::spacesToTabs(s, true); }); });

    cmds_.add(IDM_EDIT_OPENSELECTEDFILETOEDIT, [this] { cmdOpenSelectedFile(false); });
    cmds_.add(IDM_EDIT_OPENSELECTEDFILEFOLDERINEXPLORER, [this] { cmdOpenSelectedFile(true); });
    cmds_.add(IDM_EDIT_SEARCHONINTERNET, [this] {
        EditorView* v = activeView();
        if (!v)
            return;
        std::string q = v->selectedText();
        if (q.empty())
            return;
        // Preferences > Search Engine; $(CURRENT_WORD) stands for the text.
        static const char* const kEngines[] = {"https://duckduckgo.com/?q=$(CURRENT_WORD)", "https://www.google.com/search?q=$(CURRENT_WORD)",
                                               "https://www.bing.com/search?q=$(CURRENT_WORD)", "https://search.yahoo.com/search?q=$(CURRENT_WORD)"};
        std::string url = settings_.searchEngine < 4 ? kEngines[std::clamp(settings_.searchEngine, 0, 3)] : settings_.searchEngineCustom;
        if (url.empty())
            url = kEngines[0];
        gchar* esc = g_uri_escape_string(q.c_str(), nullptr, FALSE);
        const std::string token = "$(CURRENT_WORD)";
        if (const std::size_t at = url.find(token); at != std::string::npos)
            url.replace(at, token.size(), esc);
        else
            url += esc;
        g_free(esc);
        openUri(url);
    });

    cmds_.add(IDM_EDIT_MULTISELECTALL, [this] { cmdMultiSelect(true, false, false); });
    cmds_.add(IDM_EDIT_MULTISELECTALLMATCHCASE, [this] { cmdMultiSelect(true, true, false); });
    cmds_.add(IDM_EDIT_MULTISELECTALLWHOLEWORD, [this] { cmdMultiSelect(true, false, true); });
    cmds_.add(IDM_EDIT_MULTISELECTALLMATCHCASEWHOLEWORD, [this] { cmdMultiSelect(true, true, true); });
    cmds_.add(IDM_EDIT_MULTISELECTNEXT, [this] { cmdMultiSelect(false, false, false); });
    cmds_.add(IDM_EDIT_MULTISELECTNEXTMATCHCASE, [this] { cmdMultiSelect(false, true, false); });
    cmds_.add(IDM_EDIT_MULTISELECTNEXTWHOLEWORD, [this] { cmdMultiSelect(false, false, true); });
    cmds_.add(IDM_EDIT_MULTISELECTNEXTMATCHCASEWHOLEWORD, [this] { cmdMultiSelect(false, true, true); });
    cmds_.add(IDM_EDIT_MULTISELECTUNDO, [this] {
        withSci([](const core::Sci& s) {
            if (s(SCI_GETSELECTIONS) > 1)
                s(SCI_DROPSELECTIONN, static_cast<uptr_t>(s(SCI_GETMAINSELECTION)));
        });
    });
    cmds_.add(IDM_EDIT_MULTISELECTSSKIP, [this] {
        withSci([](const core::Sci& s) {
            const sptr_t n = s(SCI_GETSELECTIONS), main = s(SCI_GETMAINSELECTION);
            s(SCI_TARGETWHOLEDOCUMENT);
            s(SCI_MULTIPLESELECTADDNEXT);
            if (n > 1 && s(SCI_GETSELECTIONS) > n)
                s(SCI_DROPSELECTIONN, static_cast<uptr_t>(main));
        });
    });

    cmds_.add(IDM_EDIT_TOGGLEREADONLY, [this, buf] { if (core::Buffer* b = buf()) setReadOnly(*b, !b->userReadOnly); },
              [buf] { core::Buffer* b = buf(); return b && b->userReadOnly; });
    cmds_.add(IDM_EDIT_SETREADONLYFORALLDOCS, [this] { for (core::Buffer* b : buffers()) setReadOnly(*b, true); });
    cmds_.add(IDM_EDIT_CLEARREADONLYFORALLDOCS, [this] { for (core::Buffer* b : buffers()) setReadOnly(*b, false); });
}

void MainWindow::registerSearchCommands()
{
    cmds_.add(IDM_SEARCH_FIND, [this] { findDlg_->show(FindDialog::Tab::Find); });
    cmds_.add(IDM_SEARCH_REPLACE, [this] { findDlg_->show(FindDialog::Tab::Replace); });
    cmds_.add(IDM_SEARCH_FINDINFILES, [this] { findDlg_->show(FindDialog::Tab::FindInFiles); });
    cmds_.add(IDM_SEARCH_MARK, [this] { findDlg_->show(FindDialog::Tab::Mark); });
    cmds_.add(IDM_SEARCH_FINDNEXT, [this] { findDlg_->findNext(true); });
    cmds_.add(IDM_SEARCH_FINDPREV, [this] { findDlg_->findNext(false); });
    cmds_.add(IDM_SEARCH_SETANDFINDNEXT, [this] { findDlg_->selectAndFind(true, false); });
    cmds_.add(IDM_SEARCH_SETANDFINDPREV, [this] { findDlg_->selectAndFind(false, false); });
    cmds_.add(IDM_SEARCH_VOLATILE_FINDNEXT, [this] { findDlg_->selectAndFind(true, true); });
    cmds_.add(IDM_SEARCH_VOLATILE_FINDPREV, [this] { findDlg_->selectAndFind(false, true); });
    cmds_.add(IDM_SEARCH_FINDINCREMENT, [this] { findBar_->show(); });
    cmds_.add(IDM_FOCUS_ON_FOUND_RESULTS, [this] {
        // Upstream F7: to the results, and back to the document.
        GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(window_));
        if (dock_->isShown(kSearchResultsPanel) && focus && gtk_widget_is_ancestor(focus, results_->widget())) {
            if (EditorView* v = activeView())
                gtk_widget_grab_focus(v->widget());
            return;
        }
        showSearchResults(true);
        results_->focus();
    });
    cmds_.add(IDM_SEARCH_GOTONEXTFOUND, [this] { results_->next(true); });
    cmds_.add(IDM_SEARCH_GOTOPREVFOUND, [this] { results_->next(false); });
    cmds_.add(IDM_SEARCH_GOTOLINE, [this] { cmdGotoLine(); });
    cmds_.add(IDM_SEARCH_GOTOMATCHINGBRACE, [this] {
        withSci([](const core::Sci& s) {
            sptr_t pos = s(SCI_GETCURRENTPOS);
            sptr_t m = s(SCI_BRACEMATCH, static_cast<uptr_t>(pos), 0);
            if (m < 0 && pos > 0)
                m = s(SCI_BRACEMATCH, static_cast<uptr_t>(--pos), 0);
            if (m >= 0)
                s(SCI_GOTOPOS, static_cast<uptr_t>(m));
        });
    });
    cmds_.add(IDM_SEARCH_SELECTMATCHINGBRACES, [this] {
        // Upstream: the text between the innermost enclosing pair, braces included.
        withSci([](const core::Sci& s) {
            const sptr_t caret = s(SCI_GETCURRENTPOS);
            for (sptr_t p = caret; p-- > 0;) {
                const char c = static_cast<char>(s(SCI_GETCHARAT, static_cast<uptr_t>(p)));
                if (c != '(' && c != '[' && c != '{')
                    continue;
                const sptr_t m = s(SCI_BRACEMATCH, static_cast<uptr_t>(p), 0);
                if (m >= caret) {
                    s(SCI_SETSEL, static_cast<uptr_t>(p), m + 1);
                    return;
                }
            }
        });
    });

    // Style tokens (Mark Style 1-5).
    const int markAll[] = {IDM_SEARCH_MARKALLEXT1, IDM_SEARCH_MARKALLEXT2, IDM_SEARCH_MARKALLEXT3, IDM_SEARCH_MARKALLEXT4, IDM_SEARCH_MARKALLEXT5};
    const int markOne[] = {IDM_SEARCH_MARKONEEXT1, IDM_SEARCH_MARKONEEXT2, IDM_SEARCH_MARKONEEXT3, IDM_SEARCH_MARKONEEXT4, IDM_SEARCH_MARKONEEXT5};
    const int unmark[] = {IDM_SEARCH_UNMARKALLEXT1, IDM_SEARCH_UNMARKALLEXT2, IDM_SEARCH_UNMARKALLEXT3, IDM_SEARCH_UNMARKALLEXT4, IDM_SEARCH_UNMARKALLEXT5};
    const int prev[] = {IDM_SEARCH_GOPREVMARKER1, IDM_SEARCH_GOPREVMARKER2, IDM_SEARCH_GOPREVMARKER3, IDM_SEARCH_GOPREVMARKER4, IDM_SEARCH_GOPREVMARKER5};
    const int next[] = {IDM_SEARCH_GONEXTMARKER1, IDM_SEARCH_GONEXTMARKER2, IDM_SEARCH_GONEXTMARKER3, IDM_SEARCH_GONEXTMARKER4, IDM_SEARCH_GONEXTMARKER5};
    const int toClip[] = {IDM_SEARCH_STYLE1TOCLIP, IDM_SEARCH_STYLE2TOCLIP, IDM_SEARCH_STYLE3TOCLIP, IDM_SEARCH_STYLE4TOCLIP, IDM_SEARCH_STYLE5TOCLIP};
    for (int n = 1; n <= 5; ++n) {
        const int ind = core::markStyleIndicator(n);
        cmds_.add(markAll[n - 1], [this, n] { cmdMarkToken(n, true); });
        cmds_.add(markOne[n - 1], [this, n] { cmdMarkToken(n, false); });
        cmds_.add(unmark[n - 1], [this, ind] { withSci([ind](const core::Sci& s) { core::SearchEngine(s).clearMarks(ind); }); });
        cmds_.add(prev[n - 1], [this, ind] { cmdJumpMarker(ind, false); });
        cmds_.add(next[n - 1], [this, ind] { cmdJumpMarker(ind, true); });
        cmds_.add(toClip[n - 1], [this, ind] { cmdCopyStyled({ind}); });
    }
    cmds_.add(IDM_SEARCH_CLEARALLMARKS, [this] {
        withSci([](const core::Sci& s) {
            for (int n = 1; n <= 5; ++n)
                core::SearchEngine(s).clearMarks(core::markStyleIndicator(n));
        });
    });
    cmds_.add(IDM_SEARCH_GOPREVMARKER_DEF, [this] { cmdJumpMarker(core::kFoundIndicator, false); });
    cmds_.add(IDM_SEARCH_GONEXTMARKER_DEF, [this] { cmdJumpMarker(core::kFoundIndicator, true); });
    cmds_.add(IDM_SEARCH_ALLSTYLESTOCLIP, [this] {
        cmdCopyStyled({core::markStyleIndicator(1), core::markStyleIndicator(2), core::markStyleIndicator(3),
                       core::markStyleIndicator(4), core::markStyleIndicator(5)});
    });
    cmds_.add(IDM_SEARCH_MARKEDTOCLIP, [this] { cmdCopyStyled({core::kFoundIndicator}); });

    // Bookmarks.
    cmds_.add(IDM_SEARCH_TOGGLE_BOOKMARK, [this] { if (EditorView* v = activeView()) v->toggleBookmarkAtCaret(); });
    cmds_.add(IDM_SEARCH_NEXT_BOOKMARK, [this] { if (EditorView* v = activeView()) v->gotoNextBookmark(true); });
    cmds_.add(IDM_SEARCH_PREV_BOOKMARK, [this] { if (EditorView* v = activeView()) v->gotoNextBookmark(false); });
    cmds_.add(IDM_SEARCH_CLEAR_BOOKMARKS, [this] { withSci([](const core::Sci& s) { s(SCI_MARKERDELETEALL, kBookmarkMarker); }); });
    cmds_.add(IDM_SEARCH_CUTMARKEDLINES, [this] {
        editOp([](const core::Sci& s) {
            setClipboard(core::copyMarkedLines(s, kBookmarkMarker));
            core::deleteMarkedLines(s, kBookmarkMarker, true);
        });
    });
    cmds_.add(IDM_SEARCH_COPYMARKEDLINES, [this] { withSci([](const core::Sci& s) { setClipboard(core::copyMarkedLines(s, kBookmarkMarker)); }); });
    cmds_.add(IDM_SEARCH_PASTEMARKEDLINES, [this] {
        const std::string text = clipboardText();
        editOp([&](const core::Sci& s) { core::replaceMarkedLines(s, kBookmarkMarker, text); });
    });
    cmds_.add(IDM_SEARCH_DELETEMARKEDLINES, [this] { editOp([](const core::Sci& s) { core::deleteMarkedLines(s, kBookmarkMarker, true); }); });
    cmds_.add(IDM_SEARCH_DELETEUNMARKEDLINES, [this] { editOp([](const core::Sci& s) { core::deleteMarkedLines(s, kBookmarkMarker, false); }); });
    cmds_.add(IDM_SEARCH_INVERSEMARKS, [this] { withSci([](const core::Sci& s) { core::inverseMarks(s, kBookmarkMarker); }); });
}

void MainWindow::registerViewCommands()
{
    auto applyAll = [this] {
        for (auto& t : tabs_)
            applyViewSettings(*t->view);
    };
    auto toggle = [this, applyAll](int id, bool platform::Settings::*field) {
        cmds_.add(id, [this, applyAll, field] { settings_.*field = !(settings_.*field); applyAll(); },
                  [this, field] { return settings_.*field; });
    };
    cmds_.add(IDM_VIEW_ALWAYSONTOP, [this] {
        alwaysOnTop_ = !alwaysOnTop_;
        gtk_window_set_keep_above(GTK_WINDOW(window_), alwaysOnTop_);
    }, [this] { return alwaysOnTop_; });
    cmds_.add(IDM_VIEW_FULLSCREENTOGGLE, [this] {
        fullScreen_ = !fullScreen_;
        if (fullScreen_)
            gtk_window_fullscreen(GTK_WINDOW(window_));
        else
            gtk_window_unfullscreen(GTK_WINDOW(window_));
    }, [this] { return fullScreen_; });
    cmds_.add(IDM_VIEW_POSTIT, [this] { cmdPostIt(); }, [this] { return postIt_; });

    toggle(IDM_VIEW_TAB_SPACE, &platform::Settings::showSpaces);
    toggle(IDM_VIEW_EOL, &platform::Settings::showEol);
    toggle(IDM_VIEW_NPC, &platform::Settings::showNpc);
    toggle(IDM_VIEW_NPC_CCUNIEOL, &platform::Settings::showCcUniEol);
    // Upstream: spaces and tabs, EOL, non-printing and control characters.
    auto allShown = [this] { return settings_.showSpaces && settings_.showEol && settings_.showNpc && settings_.showCcUniEol; };
    cmds_.add(IDM_VIEW_ALL_CHARACTERS, [this, applyAll, allShown] {
        const bool on = !allShown();
        settings_.showSpaces = settings_.showEol = settings_.showNpc = settings_.showCcUniEol = on;
        applyAll();
    }, allShown);
    toggle(IDM_VIEW_INDENT_GUIDE, &platform::Settings::showIndentGuides);
    toggle(IDM_VIEW_WRAP_SYMBOL, &platform::Settings::showWrapSymbol);
    toggle(IDM_VIEW_WRAP, &platform::Settings::wordWrap);

    auto zoomFromActive = [this, applyAll] {
        if (EditorView* v = activeView())
            settings_.zoom = static_cast<int>(v->execute(SCI_GETZOOM));
        applyAll();
    };
    cmds_.add(IDM_VIEW_ZOOMIN, [this, zoomFromActive] { sciCommand(SCI_ZOOMIN); zoomFromActive(); });
    cmds_.add(IDM_VIEW_ZOOMOUT, [this, zoomFromActive] { sciCommand(SCI_ZOOMOUT); zoomFromActive(); });
    cmds_.add(IDM_VIEW_ZOOMRESTORE, [this, applyAll] { settings_.zoom = 0; applyAll(); });

    cmds_.add(IDM_VIEW_GOTO_ANOTHER_VIEW, [this] { moveToOtherView(); });
    cmds_.add(IDM_VIEW_CLONE_TO_ANOTHER_VIEW, [this] { cloneToOtherView(); });
    cmds_.add(IDM_VIEW_SWITCHTO_OTHER_VIEW, [this] { switchToOtherView(); });

    const int tabIds[] = {IDM_VIEW_TAB1, IDM_VIEW_TAB2, IDM_VIEW_TAB3, IDM_VIEW_TAB4, IDM_VIEW_TAB5,
                          IDM_VIEW_TAB6, IDM_VIEW_TAB7, IDM_VIEW_TAB8, IDM_VIEW_TAB9};
    for (int i = 0; i < 9; ++i)
        cmds_.add(tabIds[i], [this, i] {
            if (Tab* t = tabAt(activeView_, i))
                activateTab(*t);
        });
    auto cycle = [this](int delta) {
        GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[activeView_]);
        const int n = gtk_notebook_get_n_pages(nb);
        if (n > 1)
            gtk_notebook_set_current_page(nb, (gtk_notebook_get_current_page(nb) + delta + n) % n);
    };
    // Ctrl+Tab: with Preferences > MISC. "Document Switcher ... MRU", the most
    // recently used documents first, else in tab order.
    auto switcher = [this, cycle](int delta) {
        if (!settings_.docSwitcher || !settings_.docSwitcherMru || mru_.size() < 2) {
            cycle(delta);
            return;
        }
        const core::Buffer* cur = activeBuffer();
        auto it = std::find(mru_.begin(), mru_.end(), cur);
        const std::size_t n = mru_.size();
        const std::size_t i = it == mru_.end() ? 0 : static_cast<std::size_t>(it - mru_.begin());
        const core::Buffer* next = mru_[(i + n + static_cast<std::size_t>(delta > 0 ? 1 : n - 1)) % n];
        // The order holds while Ctrl is down; releasing it makes the choice the most recent.
        mruSwitching_ = true;
        for (const auto& t : tabs_)
            if (t->buf.get() == next) {
                activateTab(*t);
                break;
            }
    };
    cmds_.add(IDM_VIEW_TAB_NEXT, [switcher] { switcher(1); });
    cmds_.add(IDM_VIEW_TAB_PREV, [switcher] { switcher(-1); });
    cmds_.add(IDM_VIEW_TAB_START, [this] { if (Tab* t = tabAt(activeView_, 0)) activateTab(*t); });
    cmds_.add(IDM_VIEW_TAB_END, [this] {
        if (Tab* t = tabAt(activeView_, gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks_[activeView_])) - 1))
            activateTab(*t);
    });
    cmds_.add(IDM_VIEW_GOTO_START, [this] { moveTab(0); });
    cmds_.add(IDM_VIEW_GOTO_END, [this] { moveTab(-1); });
    auto shift = [this](int delta) {
        if (Tab* t = activeTab())
            moveTab(gtk_notebook_page_num(GTK_NOTEBOOK(notebooks_[t->viewIdx]), t->view->widget()) + delta);
    };
    cmds_.add(IDM_VIEW_TAB_MOVEFORWARD, [shift] { shift(1); });
    cmds_.add(IDM_VIEW_TAB_MOVEBACKWARD, [shift] { shift(-1); });

    cmds_.add(IDM_VIEW_FOLDALL, [this] { cmdFold(true, 0); });
    cmds_.add(IDM_VIEW_UNFOLDALL, [this] { cmdFold(false, 0); });
    cmds_.add(IDM_VIEW_FOLD_CURRENT, [this] { cmdFoldCurrent(true); });
    cmds_.add(IDM_VIEW_UNFOLD_CURRENT, [this] { cmdFoldCurrent(false); });
    const int foldIds[] = {IDM_VIEW_FOLD_1, IDM_VIEW_FOLD_2, IDM_VIEW_FOLD_3, IDM_VIEW_FOLD_4,
                           IDM_VIEW_FOLD_5, IDM_VIEW_FOLD_6, IDM_VIEW_FOLD_7, IDM_VIEW_FOLD_8};
    const int unfoldIds[] = {IDM_VIEW_UNFOLD_1, IDM_VIEW_UNFOLD_2, IDM_VIEW_UNFOLD_3, IDM_VIEW_UNFOLD_4,
                             IDM_VIEW_UNFOLD_5, IDM_VIEW_UNFOLD_6, IDM_VIEW_UNFOLD_7, IDM_VIEW_UNFOLD_8};
    for (int lv = 1; lv <= 8; ++lv) {
        cmds_.add(foldIds[lv - 1], [this, lv] { cmdFold(true, lv); });
        cmds_.add(unfoldIds[lv - 1], [this, lv] { cmdFold(false, lv); });
    }
    cmds_.add(IDM_VIEW_SUMMARY, [this] { cmdSummary(); });
}

void MainWindow::registerFormatCommands()
{
    using platform::Bom;
    using platform::TextEncoding;
    const TextEncoding utf8{"UTF-8", Bom::None}, utf8Bom{"UTF-8", Bom::Utf8};
    const TextEncoding utf16be{"UTF-16BE", Bom::Utf16BE}, utf16le{"UTF-16LE", Bom::Utf16LE};
    // Encoding > UTF-8 ... : how the document is read (upstream reloads it
    // for a non-Unicode document); "Convert to" changes what is written.
    const std::pair<int, TextEncoding> unicode[] = {
        {IDM_FORMAT_AS_UTF_8, utf8}, {IDM_FORMAT_UTF_8, utf8Bom}, {IDM_FORMAT_UTF_16BE, utf16be}, {IDM_FORMAT_UTF_16LE, utf16le}};
    for (const auto& [id, e] : unicode)
        cmds_.add(id, [this, e = e] { cmdReinterpret(e); }, [this, e = e] { return currentEncodingIs(e); });
    const std::pair<int, TextEncoding> conv[] = {
        {IDM_FORMAT_CONV2_AS_UTF_8, utf8}, {IDM_FORMAT_CONV2_UTF_8, utf8Bom},
        {IDM_FORMAT_CONV2_UTF_16BE, utf16be}, {IDM_FORMAT_CONV2_UTF_16LE, utf16le}};
    for (const auto& [id, e] : conv)
        cmds_.add(id, [this, e = e] { cmdConvertEncoding(e); });
    // "ANSI" is the system code page, UTF-8 on Linux: upstream does nothing
    // for it on a UTF-8 system, so the ANSI items are left out.
    for (int id = IDM_FORMAT_ENCODE; id <= IDM_FORMAT_ENCODE_END; ++id) {
        const int cp = kEncodeCodepages[id - IDM_FORMAT_ENCODE];
        const std::string cs = cp < 0 ? std::string() : platform::charsetForCodepage(cp);
        if (cs.empty())
            continue;
        const TextEncoding e{cs, Bom::None};
        cmds_.add(id, [this, e] { cmdReinterpret(e); }, [this, e] { return currentEncodingIs(e); });
    }
}

void MainWindow::registerOtherCommands()
{
    cmds_.add(kCmdImportConfig, [this] { cmdImportConfig(); });
    cmds_.add(IDM_LANG_OPENUDLDIR, [this] {
        const fs::path dir = platform::userConfigDir() / "userDefineLangs";
        std::error_code ec;
        fs::create_directories(dir, ec);
        gchar* uri = g_filename_to_uri(dir.c_str(), nullptr, nullptr);
        if (uri)
            openUri(uri);
        g_free(uri);
    });
    cmds_.add(IDM_SETTING_IMPORTSTYLETHEMES, [this] {
        const fs::path f = chooseFile(GTK_WINDOW(window_), "Import style theme", GTK_FILE_CHOOSER_ACTION_OPEN, "_Import",
                                      lastDir_, "*.xml", "Notepad++ themes (*.xml)");
        if (f.empty())
            return;
        std::error_code ec;
        const fs::path dir = platform::userConfigDir() / "themes";
        fs::create_directories(dir, ec);
        fs::copy_file(f, dir / f.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec)
            showError("Cannot import the theme", ec.message());
        refreshThemeMenu();
    });
    auto hash = [this](GChecksumType type) {
        EditorView* v = activeView();
        if (!v)
            return;
        const std::string sel = v->selectedText();
        if (!sel.empty())
            setClipboard(checksum(type, sel));
    };
    cmds_.add(IDM_TOOL_MD5_GENERATEINTOCLIPBOARD, [hash] { hash(G_CHECKSUM_MD5); });
    cmds_.add(IDM_TOOL_SHA1_GENERATEINTOCLIPBOARD, [hash] { hash(G_CHECKSUM_SHA1); });
    cmds_.add(IDM_TOOL_SHA256_GENERATEINTOCLIPBOARD, [hash] { hash(G_CHECKSUM_SHA256); });
    cmds_.add(IDM_TOOL_SHA512_GENERATEINTOCLIPBOARD, [hash] { hash(G_CHECKSUM_SHA512); });

    cmds_.add(IDM_HOMESWEETHOME, [this] { openUri("https://notepad-plus-plus.org/"); });
    cmds_.add(IDM_PROJECTPAGE, [this] { openUri("https://github.com/notepad-plus-plus/notepad-plus-plus/"); });
    cmds_.add(IDM_ONLINEDOCUMENT, [this] { openUri("https://npp-user-manual.org/"); });
    cmds_.add(IDM_FORUM, [this] { openUri("https://community.notepad-plus-plus.org/"); });
    cmds_.add(IDM_ABOUT, [this] { cmdAbout(); });
}

// ============================================================================
// File commands
// ============================================================================

void MainWindow::cmdOpen()
{
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(core::Translator::plain(tr_.command(IDM_FILE_OPEN, "Open")).c_str(),
                                                            GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    if (const fs::path dir = dialogFolder(); !dir.empty())
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), dir.c_str());
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        std::vector<fs::path> files;
        GSList* list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        for (GSList* l = list; l; l = l->next) {
            files.emplace_back(static_cast<char*>(l->data));
            g_free(l->data);
        }
        g_slist_free(list);
        openFiles(files);
    }
    g_object_unref(dlg);
}

bool MainWindow::saveCopyAs(core::Buffer& b)
{
    const std::vector<Tab*> tabs = tabsOf(b);
    if (tabs.empty())
        return false;
    const fs::path f = chooseFile(GTK_WINDOW(window_), core::Translator::plain(tr_.command(IDM_FILE_SAVECOPYAS, "Save a Copy As")),
                                  GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", b.isUntitled() ? dialogFolder() : b.path.parent_path(), nullptr, nullptr);
    if (f.empty())
        return false;
    std::string err;
    bool unrepresentable = false;
    if (!core::saveFile(f, tabs.front()->view->text(), b.encoding, &err, &unrepresentable)) {
        showError("Cannot save \"" + f.filename().string() + "\"", err);
        return false;
    }
    return true;
}

void MainWindow::renameBuffer(core::Buffer& b)
{
    std::string name = b.name();
    if (!promptText(GTK_WINDOW(window_), core::Translator::plain(tr_.command(IDM_FILE_RENAME, "Rename")),
                    b.isUntitled() ? "New name:" : "New file name:", name)
        || name.empty() || name == b.name() || name.find('/') != std::string::npos)
        return;
    notifyPlugins(NPPN_FILEBEFORERENAME, &b);
    if (b.isUntitled()) { // upstream renames the tab of an untitled document
        b.untitledName = name;
    } else {
        const fs::path target = b.path.parent_path() / name;
        std::error_code ec;
        if (fs::exists(target, ec)) {
            notifyPlugins(NPPN_FILERENAMECANCEL, &b);
            showError("Cannot rename \"" + b.name() + "\"", target.string() + " already exists.");
            return;
        }
        fs::rename(b.path, target, ec);
        if (ec) {
            notifyPlugins(NPPN_FILERENAMECANCEL, &b);
            showError("Cannot rename \"" + b.name() + "\"", ec.message());
            return;
        }
        b.path = target;
        b.diskStamp = platform::statFile(target);
        watch(b);
        if (!b.userLanguage && (!b.language || b.language->isPlainText()))
            detectLanguage(b, target);
    }
    refreshBufferTabs(b);
    refreshAll();
    notifyPlugins(NPPN_FILERENAMED, &b);
}

void MainWindow::cmdLoadSession()
{
    const fs::path f = chooseFile(GTK_WINDOW(window_), core::Translator::plain(tr_.command(IDM_FILE_LOADSESSION, "Load Session")),
                                  GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", lastDir_, "*.xml", "Session files (*.xml)");
    std::string err;
    if (!f.empty() && !loadSession(f, &err))
        showError("Cannot load session", err);
}

void MainWindow::cmdSaveSession()
{
    const fs::path f = chooseFile(GTK_WINDOW(window_), core::Translator::plain(tr_.command(IDM_FILE_SAVESESSION, "Save Session")),
                                  GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", lastDir_, "*.xml", "Session files (*.xml)");
    std::string err;
    if (!f.empty() && !saveSession(f, &err))
        showError("Cannot save session", err);
}

void MainWindow::cmdImportConfig()
{
    const fs::path f = chooseFile(GTK_WINDOW(window_), "Import Notepad++ config.xml", GTK_FILE_CHOOSER_ACTION_OPEN, "_Import",
                                  platform::userConfigDir(), "config*.xml", "Notepad++ configuration (config.xml)");
    if (f.empty())
        return;
    std::string err;
    auto cfg = core::readNppConfig(f, &err);
    if (!cfg) {
        showError("Cannot import settings", err);
        return;
    }
    const std::string oldTheme = settings_.theme;
    const int n = core::applyNppConfig(*cfg, settings_);
    recent_.setMax(static_cast<std::size_t>(std::max(settings_.maxRecentFiles, 0)));
    recent_.assign(settings_.recentFiles);
    refreshRecentMenu();
    if (settings_.theme != oldTheme)
        cmdSetTheme(settings_.theme);
    for (auto& t : tabs_)
        applyViewSettings(*t->view);
    rewatchAll();
    restartSnapshotTimer();
    refreshAll();
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                            "Imported %d setting%s from %s.", n, n == 1 ? "" : "s", f.filename().c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

void MainWindow::cmdRestoreRecent()
{
    if (!recent_.empty())
        openRecentFile(recent_.items().front());
}

void MainWindow::cmdOpenAllRecent()
{
    std::vector<fs::path> files;
    for (const std::string& f : recent_.items()) {
        std::error_code ec;
        if (fs::is_regular_file(f, ec))
            files.emplace_back(f);
    }
    openFiles(files);
}

void MainWindow::openRecentFile(const std::string& file)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        if (ask("\"" + file + "\" doesn't exist anymore.", "Remove it from the recent files list?", false) == Answer::Yes) {
            recent_.remove(file);
            refreshRecentMenu();
        }
        return;
    }
    openFiles({file});
}

void MainWindow::showTabMenu(Tab& t, const GdkEvent* ev)
{
    activateTab(t); // the menu acts on the active document
    gtk_menu_popup_at_pointer(GTK_MENU(tabMenu_), ev);
}

void MainWindow::openUriList(std::string_view uris)
{
    std::vector<fs::path> files;
    gchar** list = g_uri_list_extract_uris(std::string(uris).c_str());
    for (gchar** u = list; u && *u; ++u)
        if (gchar* fn = g_filename_from_uri(*u, nullptr, nullptr)) {
            files.emplace_back(fn);
            g_free(fn);
        }
    g_strfreev(list);
    if (!files.empty())
        openFiles(files);
}

// ============================================================================
// Edit / Search / View commands
// ============================================================================

void MainWindow::cmdGotoLine()
{
    EditorView* v = activeView();
    if (!v)
        return;
    GtkWidget* dlg = gtk_dialog_new_with_buttons(tr_.dialogAttr("GoToLine", "title", "Go To...").c_str(), GTK_WINDOW(window_),
                                                 GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL, "_Go", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    char buf[96];
    g_snprintf(buf, sizeof buf, "(1 - %ld)", static_cast<long>(v->lineCount()));
    const std::string label = core::Translator::plain(tr_.dialog("GoToLine", 2007, "Go to line:")) + " " + buf;
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label.c_str()), FALSE, FALSE, 4);
    GtkWidget* spin = gtk_spin_button_new_with_range(1, static_cast<double>(v->lineCount()), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), static_cast<double>(v->lineFromPos(v->currentPos()) + 1));
    gtk_entry_set_activates_default(GTK_ENTRY(spin), TRUE);
    gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 4);
    gtk_widget_show_all(box);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gtk_spin_button_update(GTK_SPIN_BUTTON(spin));
        v->gotoLine(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin)) - 1);
    }
    gtk_widget_destroy(dlg);
    gtk_widget_grab_focus(v->widget());
}

void MainWindow::cmdSort(int id)
{
    core::SortKind kind = core::SortKind::Lexicographic;
    bool desc = false;
    switch (id) {
    case IDM_EDIT_SORTLINES_LEXICOGRAPHIC_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_LEXICOGRAPHIC_ASCENDING: kind = core::SortKind::Lexicographic; break;
    case IDM_EDIT_SORTLINES_LEXICO_CASE_INSENS_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_LEXICO_CASE_INSENS_ASCENDING: kind = core::SortKind::LexicographicIgnoreCase; break;
    case IDM_EDIT_SORTLINES_LOCALE_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_LOCALE_ASCENDING: kind = core::SortKind::Locale; break;
    case IDM_EDIT_SORTLINES_INTEGER_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_INTEGER_ASCENDING: kind = core::SortKind::Integer; break;
    case IDM_EDIT_SORTLINES_DECIMALCOMMA_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_DECIMALCOMMA_ASCENDING: kind = core::SortKind::DecimalComma; break;
    case IDM_EDIT_SORTLINES_DECIMALDOT_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_DECIMALDOT_ASCENDING: kind = core::SortKind::DecimalDot; break;
    case IDM_EDIT_SORTLINES_LENGTH_DESCENDING: desc = true; [[fallthrough]];
    case IDM_EDIT_SORTLINES_LENGTH_ASCENDING: kind = core::SortKind::Length; break;
    case IDM_EDIT_SORTLINES_REVERSE_ORDER: kind = core::SortKind::Reverse; break;
    default: kind = core::SortKind::Random; break;
    }
    std::string badLine;
    editOp([&](const core::Sci& s) { core::sortLines(s, kind, desc, &badLine); });
    if (!badLine.empty())
        showError(trMessage("SortingError", "title", "Sorting Error"),
                  core::Translator::format(trMessage("SortingError", "message", "Unable to perform numeric sorting due to line $INT_REPLACE$."),
                                           "$INT_REPLACE$", badLine));
}

void MainWindow::cmdSetEol(int mode)
{
    EditorView* v = activeView();
    if (!v || v->execute(SCI_GETREADONLY))
        return;
    v->setEolMode(mode, true);
    refreshStatus();
}

void MainWindow::cmdBeginEndSelect(bool column)
{
    EditorView* v = activeView();
    if (!v)
        return;
    const sptr_t pos = v->currentPos();
    if (beginSelectPos_ < 0) {
        beginSelectPos_ = pos;
        return;
    }
    if (column) {
        v->execute(SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE);
        v->execute(SCI_SETRECTANGULARSELECTIONANCHOR, static_cast<uptr_t>(beginSelectPos_));
        v->execute(SCI_SETRECTANGULARSELECTIONCARET, static_cast<uptr_t>(pos));
    } else {
        v->execute(SCI_SETSEL, static_cast<uptr_t>(beginSelectPos_), pos);
    }
    beginSelectPos_ = -1;
}

void MainWindow::cmdOpenSelectedFile(bool folder)
{
    EditorView* v = activeView();
    core::Buffer* b = activeBuffer();
    if (!v || !b)
        return;
    std::string name = v->selectedText();
    const auto trimPos = name.find_last_not_of(" \t\r\n\"'");
    name = trimPos == std::string::npos ? "" : name.substr(0, trimPos + 1);
    name.erase(0, name.find_first_not_of(" \t\r\n\"'"));
    if (name.empty())
        return;
    fs::path p = name;
    if (p.is_relative() && !b->isUntitled())
        p = b->path.parent_path() / p;
    std::error_code ec;
    if (folder) {
        if (!fs::exists(p, ec))
            return;
        gchar* uri = g_filename_to_uri((fs::is_directory(p, ec) ? p : p.parent_path()).c_str(), nullptr, nullptr);
        if (uri)
            openUri(uri);
        g_free(uri);
    } else if (fs::is_regular_file(p, ec)) {
        openFiles({p});
    } else {
        showError("Cannot open file", p.string());
    }
}

void MainWindow::cmdMultiSelect(bool all, bool matchCase, bool wholeWord)
{
    withSci([&](const core::Sci& s) {
        s(SCI_TARGETWHOLEDOCUMENT);
        s(SCI_SETSEARCHFLAGS, (matchCase ? SCFIND_MATCHCASE : 0) | (wholeWord ? SCFIND_WHOLEWORD : 0));
        s(all ? SCI_MULTIPLESELECTADDEACH : SCI_MULTIPLESELECTADDNEXT);
        if (!all)
            s(SCI_SCROLLCARET);
    });
}

void MainWindow::cmdMarkToken(int style, bool all)
{
    // Upstream: the selection, or the word at the caret; every occurrence,
    // or just this one.
    withSci([&](const core::Sci& s) {
        sptr_t a = s(SCI_GETSELECTIONSTART), b = s(SCI_GETSELECTIONEND);
        if (a == b) {
            a = s(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(a), true);
            b = s(SCI_WORDENDPOSITION, static_cast<uptr_t>(b), true);
        }
        if (a == b)
            return;
        const int ind = core::markStyleIndicator(style);
        if (!all) {
            s(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(ind));
            s(SCI_INDICATORFILLRANGE, static_cast<uptr_t>(a), b - a);
            return;
        }
        core::FindOptions o;
        o.what = s.range(a, b);
        // Preferences > Highlighting > Style All Occurrences of Token.
        o.matchCase = settings_.markMatchCase;
        o.wholeWord = settings_.markWholeWord;
        core::SearchEngine(s).markAll(o, ind);
    });
}

void MainWindow::cmdJumpMarker(int ind, bool forward)
{
    withSci([&](const core::Sci& s) {
        const sptr_t len = s.length();
        const sptr_t caret = s(SCI_GETCURRENTPOS);
        auto valueAt = [&](sptr_t p) { return s(SCI_INDICATORVALUEAT, static_cast<uptr_t>(ind), p) != 0; };
        sptr_t target = -1;
        if (forward) {
            sptr_t p = valueAt(caret) ? s(SCI_INDICATOREND, static_cast<uptr_t>(ind), caret) : caret;
            if (p < len && !valueAt(p))
                p = s(SCI_INDICATOREND, static_cast<uptr_t>(ind), p);
            if (p < len && valueAt(p))
                target = p;
            else if (valueAt(0)) // wrap
                target = 0;
            else if (sptr_t q = s(SCI_INDICATOREND, static_cast<uptr_t>(ind), 0); q < len && valueAt(q))
                target = q;
        } else {
            sptr_t p = caret > 0 ? caret - 1 : 0;
            if (valueAt(p))
                p = s(SCI_INDICATORSTART, static_cast<uptr_t>(ind), p) - 1;
            while (p >= 0 && !valueAt(p))
                p = s(SCI_INDICATORSTART, static_cast<uptr_t>(ind), p) - 1;
            if (p >= 0) {
                target = s(SCI_INDICATORSTART, static_cast<uptr_t>(ind), p);
            } else if (len > 0) { // wrap to the last one
                p = len - 1;
                while (p >= 0 && !valueAt(p))
                    p = s(SCI_INDICATORSTART, static_cast<uptr_t>(ind), p) - 1;
                if (p >= 0)
                    target = s(SCI_INDICATORSTART, static_cast<uptr_t>(ind), p);
            }
        }
        if (target < 0)
            return;
        const sptr_t end = s(SCI_INDICATOREND, static_cast<uptr_t>(ind), target);
        s(SCI_SETSEL, static_cast<uptr_t>(target), end);
        s(SCI_SCROLLCARET);
    });
}

void MainWindow::cmdCopyStyled(const std::vector<int>& indicators)
{
    withSci([&](const core::Sci& s) {
        // Upstream: every styled run in document order, one per line.
        std::vector<std::pair<sptr_t, sptr_t>> runs;
        const sptr_t len = s.length();
        for (int ind : indicators)
            for (sptr_t p = 0; p < len;) {
                const sptr_t e = s(SCI_INDICATOREND, static_cast<uptr_t>(ind), p);
                if (s(SCI_INDICATORVALUEAT, static_cast<uptr_t>(ind), p))
                    runs.emplace_back(p, e);
                if (e <= p)
                    break;
                p = e;
            }
        std::sort(runs.begin(), runs.end());
        std::string out;
        for (const auto& [a, b] : runs)
            out += s.range(a, b) + "\n";
        if (!out.empty())
            setClipboard(out);
    });
}

void MainWindow::cmdFold(bool fold, int level)
{
    withSci([&](const core::Sci& s) {
        if (level == 0) {
            s(SCI_FOLDALL, fold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
            return;
        }
        for (sptr_t l = 0, n = s.lineCount(); l < n; ++l) {
            const sptr_t lv = s(SCI_GETFOLDLEVEL, static_cast<uptr_t>(l));
            if ((lv & SC_FOLDLEVELHEADERFLAG) && (lv & SC_FOLDLEVELNUMBERMASK) - SC_FOLDLEVELBASE == level - 1)
                s(SCI_FOLDLINE, static_cast<uptr_t>(l), fold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
        }
    });
}

void MainWindow::cmdFoldCurrent(bool fold)
{
    withSci([&](const core::Sci& s) {
        sptr_t line = s.lineOf(s(SCI_GETCURRENTPOS));
        if (!(s(SCI_GETFOLDLEVEL, static_cast<uptr_t>(line)) & SC_FOLDLEVELHEADERFLAG))
            line = s(SCI_GETFOLDPARENT, static_cast<uptr_t>(line));
        if (line < 0)
            return;
        // Preferences > Editing 1: the commands toggle the current level.
        if (settings_.foldToggle)
            s(SCI_FOLDLINE, static_cast<uptr_t>(line), SC_FOLDACTION_TOGGLE);
        else
            s(SCI_FOLDLINE, static_cast<uptr_t>(line), fold ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
    });
}

void MainWindow::cmdPostIt()
{
    // Upstream Post-It: no menu, toolbar, status bar or tab bar.
    postIt_ = !postIt_;
    for (GtkWidget* w : {menubar_, toolbar_, statusBar_})
        if (w)
            gtk_widget_set_visible(w, !postIt_);
    for (GtkWidget* nb : notebooks_)
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(nb), !postIt_);
    gtk_window_set_decorated(GTK_WINDOW(window_), !postIt_);
}

void MainWindow::cmdSummary()
{
    core::Buffer* b = activeBuffer();
    EditorView* v = activeView();
    if (!b || !v)
        return;
    const std::string text = v->text();
    long words = 0, chars = 0;
    bool inWord = false;
    for (const char* p = text.c_str(); *p; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        if (!g_unichar_isspace(c))
            ++chars;
        const bool w = g_unichar_isalnum(c) || c == '_';
        if (w && !inWord)
            ++words;
        inWord = w;
    }
    std::string info;
    if (!b->isUntitled()) {
        info += "Full file path: " + b->path.string() + "\n";
        GFile* f = g_file_new_for_path(b->path.c_str());
        if (GFileInfo* fi = g_file_query_info(f, "time::created,time::modified", G_FILE_QUERY_INFO_NONE, nullptr, nullptr)) {
            if (GDateTime* m = g_file_info_get_modification_date_time(fi)) {
                gchar* s = g_date_time_format(m, "%x %X");
                info += std::string("Modified: ") + (s ? s : "") + "\n";
                g_free(s);
                g_date_time_unref(m);
            }
            g_object_unref(fi);
        }
        g_object_unref(f);
        info += "\n";
    }
    const long length = static_cast<long>(g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size())));
    info += "Characters (without line endings): " + std::to_string(chars) + "\n";
    info += "Words: " + std::to_string(words) + "\n";
    info += "Lines: " + std::to_string(v->lineCount()) + "\n";
    info += "Document length: " + std::to_string(length) + "\n";
    const sptr_t sel = v->execute(SCI_COUNTCHARACTERS, static_cast<uptr_t>(v->execute(SCI_GETSELECTIONSTART)), v->execute(SCI_GETSELECTIONEND));
    info += std::to_string(static_cast<long>(sel)) + " selected characters (" + std::to_string(v->selectedText().size()) + " bytes)";
    GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s",
                                            core::Translator::plain(tr_.command(IDM_VIEW_SUMMARY, "Summary")).c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", info.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

// ============================================================================
// Settings / Encoding / Help
// ============================================================================

void MainWindow::cmdSetTheme(const std::string& theme)
{
    settings_.theme = theme;
    loadStyles();
    results_->applyStyles(styles_);
    terminal_->applyStyles(styles_);
    panelSource_ = nullptr; // the Document Map takes the new styles
    refreshPanels();
    for (core::Buffer* b : buffers())
        reapplyLanguage(*b);
}

void MainWindow::cmdConvertEncoding(const platform::TextEncoding& e)
{
    core::Buffer* b = activeBuffer();
    if (!b || e == b->encoding)
        return;
    // Text is held as UTF-8, so the bytes on disk change on the next save.
    b->encoding = e;
    markForcedDirty(*b, true);
    refreshStatus();
}

void MainWindow::cmdReinterpret(const platform::TextEncoding& e)
{
    core::Buffer* b = activeBuffer();
    if (!b || currentEncodingIs(e))
        return;
    const bool wasUnicode = b->encoding.bom != platform::Bom::None || g_ascii_strcasecmp(b->encoding.charset.c_str(), "UTF-8") == 0;
    const bool toUnicode = e.bom != platform::Bom::None || g_ascii_strcasecmp(e.charset.c_str(), "UTF-8") == 0;
    // Untitled, or between Unicode forms: the text stays, only the way it is
    // written changes (upstream sets the Unicode mode and marks it dirty).
    if (b->isUntitled() || (wasUnicode && toUnicode)) {
        cmdConvertEncoding(e);
        return;
    }
    // Otherwise the file is read again in that encoding, as upstream does.
    if (isModified(*b)) {
        if (ask(trMessage("SaveCurrentModifWarning", "title", "Save Current Modification"),
                trMessage("SaveCurrentModifWarning", "message",
                          "You should save the current modification.\nAll the saved modifications cannot be undone.\n\nContinue?"),
                false) != Answer::Yes
            || !save(*b, false))
            return;
    }
    const int cp = e.bom == platform::Bom::Utf16LE ? 1200
                 : e.bom == platform::Bom::Utf16BE ? 1201
                 : g_ascii_strcasecmp(e.charset.c_str(), "UTF-8") == 0 ? 65001
                                                                       : platform::codepageForCharset(e.charset);
    std::string err;
    if (!reloadBuffer(*b, &err, cp)) {
        showError("Cannot reload file", err);
        return;
    }
    if (e.bom != platform::Bom::None && b->encoding.bom != e.bom)
        cmdConvertEncoding(e);
}

void MainWindow::cmdAbout()
{
    const char* authors[] = {"Don Ho and the Notepad++ contributors (upstream)", "Linux GTK port contributors", nullptr};
    gtk_show_about_dialog(GTK_WINDOW(window_),
                          "program-name", "Notepad++ (GTK port preview)",
                          "version", NPP_VERSION,
                          "comments", "Unofficial native Linux build of the Notepad++ source tree.\n"
                                      "Editor component: Scintilla " NPP_SCINTILLA_VERSION ", Lexilla " NPP_LEXILLA_VERSION
                                      " (GTK 3 backend).",
                          "website", "https://notepad-plus-plus.org/",
                          "license-type", GTK_LICENSE_GPL_3_0,
                          "logo-icon-name", NPP_APP_ID,
                          "authors", authors,
                          nullptr);
}

} // namespace npp::ui
