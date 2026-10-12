#include "ui/Commands.h"

#include "ui/KeyMap.h"

#include "menuCmdID.h"

#include <initializer_list>

namespace npp::ui {

// ============================================================================
// CommandTable
// ============================================================================

void CommandTable::add(int id, Fn run, State checked)
{
    Cmd& c = cmds_[id];
    c.run = std::move(run);
    c.checked = std::move(checked);
}

bool CommandTable::run(int id)
{
    auto it = cmds_.find(id);
    if (it == cmds_.end() || !isEnabled(id))
        return false;
    if (onBeforeRun)
        onBeforeRun(id);
    it->second.run();
    if (onRun)
        onRun(id);
    syncStates();
    return true;
}

void CommandTable::onActivate(GSimpleAction* a, GVariant*, gpointer self)
{
    auto* t = static_cast<CommandTable*>(self);
    if (t->syncing_)
        return;
    const char* name = g_action_get_name(G_ACTION(a));
    t->run(std::atoi(name + 2)); // "id<N>"
}

void CommandTable::install(GActionMap* map)
{
    for (auto& [id, c] : cmds_) {
        if (c.action)
            continue;
        const std::string name = actionName(id);
        if (c.checked) {
            c.action = g_simple_action_new_stateful(name.c_str(), nullptr, g_variant_new_boolean(c.checked()));
            // Toggling goes through run(); the state is re-read afterwards.
            g_signal_connect(c.action, "change-state", G_CALLBACK(+[](GSimpleAction* a, GVariant*, gpointer self) {
                                 onActivate(a, nullptr, self);
                             }),
                             this);
        } else {
            c.action = g_simple_action_new(name.c_str(), nullptr);
            g_signal_connect(c.action, "activate", G_CALLBACK(&CommandTable::onActivate), this);
        }
        g_action_map_add_action(map, G_ACTION(c.action));
        g_object_unref(c.action); // the map owns it
    }
}

void CommandTable::syncStates()
{
    syncing_ = true;
    for (auto& [id, c] : cmds_)
        if (c.action && c.checked) {
            const bool on = c.checked();
            GVariant* cur = g_action_get_state(G_ACTION(c.action));
            if (!cur || g_variant_get_boolean(cur) != on)
                g_simple_action_set_state(c.action, g_variant_new_boolean(on));
            if (cur)
                g_variant_unref(cur);
        }
    syncing_ = false;
}

void CommandTable::setEnabled(int id, bool enabled)
{
    if (auto it = cmds_.find(id); it != cmds_.end() && it->second.action)
        g_simple_action_set_enabled(it->second.action, enabled);
}

bool CommandTable::isEnabled(int id) const
{
    auto it = cmds_.find(id);
    return it != cmds_.end() && (!it->second.action || g_action_get_enabled(G_ACTION(it->second.action)));
}

void CommandTable::applyAccels(GtkApplication* app, const core::CommandKeys& keys,
                               const std::vector<std::pair<std::string, core::KeyCombo>>& extra)
{
    static const char* const none[] = {nullptr};
    for (const std::string& a : accelActions_)
        gtk_application_set_accels_for_action(app, a.c_str(), none);
    accelActions_.clear();

    std::map<std::string, std::vector<std::string>> byAction;
    for (const auto& [id, c] : cmds_)
        for (const core::KeyCombo& k : keys.keysOf(id))
            for (const std::string& a : accelsFor(k))
                byAction[detailedName(id)].push_back(a);
    for (const auto& [action, k] : extra)
        for (const std::string& a : accelsFor(k))
            byAction[action].push_back(a);
    // Zoom also on the main keyboard's +/-, as GTK users expect.
    if (has(IDM_VIEW_ZOOMIN))
        for (const char* a : {"<Primary>plus", "<Primary>equal"})
            byAction[detailedName(IDM_VIEW_ZOOMIN)].push_back(a);
    if (has(IDM_VIEW_ZOOMOUT))
        byAction[detailedName(IDM_VIEW_ZOOMOUT)].push_back("<Primary>minus");

    for (auto& [action, accels] : byAction) {
        std::vector<const char*> v;
        for (const std::string& a : accels)
            v.push_back(a.c_str());
        v.push_back(nullptr);
        gtk_application_set_accels_for_action(app, action.c_str(), v.data());
        accelActions_.push_back(action);
    }
}

// ============================================================================
// Menu specification (upstream PowerEditor/src/Notepad_plus.rc)
// ============================================================================

namespace {

struct Node {
    enum Kind { Cmd, Sub, Sep, Dyn };
    Kind kind;
    int id = 0;                 // Cmd
    const char* key = nullptr;  // Sub: nativeLang subMenuId; Dyn: section name
    const char* text = nullptr; // English fallback
    std::vector<Node> kids;
    bool portLabel = false;     // Windows-specific upstream text: the port's own label wins
};

Node C(int id, const char* text) { return {Node::Cmd, id, nullptr, text, {}}; }
Node P(int id, const char* text) { return {Node::Cmd, id, nullptr, text, {}, true}; }

std::string label(const core::Translator& tr, const Node& n)
{
    return n.portLabel ? std::string(n.text) : tr.command(n.id, n.text);
}
Node S(const char* subId, const char* text, std::vector<Node> kids) { return {Node::Sub, 0, subId, text, std::move(kids)}; }
Node D(const char* name) { return {Node::Dyn, 0, name, nullptr, {}}; }
const Node SEP{Node::Sep, 0, nullptr, nullptr, {}};

struct Top {
    const char* menuId;
    const char* text;
    std::vector<Node> items;
};

std::vector<Top> mainMenu()
{
    return {
        {"file", "&File", {
            C(IDM_FILE_NEW, "&New"), C(IDM_FILE_OPEN, "&Open..."),
            S("file-openFolder", "Open Containing &Folder", {P(IDM_FILE_OPEN_FOLDER, "File Manager"), P(IDM_FILE_OPEN_CMD, "Terminal"), SEP,
                                                             C(IDM_FILE_CONTAININGFOLDERASWORKSPACE, "Folder as Workspace")}),
            C(IDM_FILE_OPEN_DEFAULT_VIEWER, "Open in &Default Viewer"),
            C(IDM_FILE_OPENFOLDERASWORKSPACE, "Open Folder as &Workspace..."),
            C(IDM_FILE_RELOAD, "Re&load from Disk"), C(IDM_FILE_SAVE, "&Save"), C(IDM_FILE_SAVEAS, "Save &As..."),
            C(IDM_FILE_SAVECOPYAS, "Save a Cop&y As..."), C(IDM_FILE_SAVEALL, "Sa&ve All"), C(IDM_FILE_RENAME, "&Rename..."),
            C(IDM_FILE_CLOSE, "&Close"), C(IDM_FILE_CLOSEALL, "Clos&e All"),
            S("file-closeMore", "Close &Multiple Documents", {
                C(IDM_FILE_CLOSEALL_BUT_CURRENT, "Close All but Active Document"),
                C(IDM_FILE_CLOSEALL_TOLEFT, "Close All to the Left"), C(IDM_FILE_CLOSEALL_TORIGHT, "Close All to the Right"),
                C(IDM_FILE_CLOSEALL_UNCHANGED, "Close All Unchanged")}),
            P(IDM_FILE_DELETE, "Move to &Trash"), SEP,
            C(IDM_FILE_LOADSESSION, "Load Sess&ion..."), C(IDM_FILE_SAVESESSION, "Save Sess&ion..."), SEP,
            C(IDM_FILE_PRINT, "&Print..."), C(IDM_FILE_PRINTNOW, "Print No&w"), SEP,
            C(IDM_FILE_RESTORELASTCLOSEDFILE, "Restore Recent Closed File"), C(IDM_OPEN_ALL_RECENT_FILE, "Open All Recent Files"),
            C(IDM_CLEAN_RECENT_FILE_LIST, "Empty Recent Files List"), D("recent"), SEP,
            C(IDM_FILE_EXIT, "E&xit")}},
        {"edit", "&Edit", {
            C(IDM_EDIT_UNDO, "&Undo"), C(IDM_EDIT_REDO, "&Redo"), SEP,
            C(IDM_EDIT_CUT, "Cu&t"), C(IDM_EDIT_COPY, "&Copy"), C(IDM_EDIT_PASTE, "&Paste"), C(IDM_EDIT_DELETE, "&Delete"),
            C(IDM_EDIT_SELECTALL, "&Select All"), C(IDM_EDIT_BEGINENDSELECT, "Begin/End &Select"),
            C(IDM_EDIT_BEGINENDSELECT_COLUMNMODE, "Begin/End Select in Column Mode"), SEP,
            S("edit-insert", "Insert", {C(IDM_EDIT_INSERT_DATETIME_SHORT, "Date Time (short)"),
                                        C(IDM_EDIT_INSERT_DATETIME_LONG, "Date Time (long)")}),
            S("edit-copyToClipboard", "Cop&y to Clipboard", {
                C(IDM_EDIT_FULLPATHTOCLIP, "Copy Current Full File path"), C(IDM_EDIT_FILENAMETOCLIP, "Copy Current Filename"),
                C(IDM_EDIT_CURRENTDIRTOCLIP, "Copy Current Dir. Path"), SEP,
                C(IDM_EDIT_COPY_ALL_NAMES, "Copy All Filenames"), C(IDM_EDIT_COPY_ALL_PATHS, "Copy All File Paths")}),
            S("edit-indent", "&Indent", {C(IDM_EDIT_INS_TAB, "Increase Line Indent"), C(IDM_EDIT_RMV_TAB, "Decrease Line Indent")}),
            S("edit-convertCaseTo", "Con&vert Case to", {
                C(IDM_EDIT_UPPERCASE, "&UPPERCASE"), C(IDM_EDIT_LOWERCASE, "&lowercase"),
                C(IDM_EDIT_PROPERCASE_FORCE, "&Proper Case"), C(IDM_EDIT_PROPERCASE_BLEND, "Proper Case (blend)"),
                C(IDM_EDIT_SENTENCECASE_FORCE, "&Sentence case"), C(IDM_EDIT_SENTENCECASE_BLEND, "Sentence case (blend)"),
                C(IDM_EDIT_INVERTCASE, "&iNVERT cASE"), C(IDM_EDIT_RANDOMCASE, "&ranDOm CasE")}),
            S("edit-lineOperations", "&Line Operations", {
                C(IDM_EDIT_DUP_LINE, "Duplicate Current Line"), C(IDM_EDIT_REMOVE_ANY_DUP_LINES, "Remove Duplicate Lines"),
                C(IDM_EDIT_REMOVE_CONSECUTIVE_DUP_LINES, "Remove Consecutive Duplicate Lines"),
                C(IDM_EDIT_SPLIT_LINES, "Split Lines"), C(IDM_EDIT_JOIN_LINES, "Join Lines"),
                C(IDM_EDIT_LINE_UP, "Move Up Current Line"), C(IDM_EDIT_LINE_DOWN, "Move Down Current Line"),
                C(IDM_EDIT_REMOVEEMPTYLINES, "Remove Empty Lines"),
                C(IDM_EDIT_REMOVEEMPTYLINESWITHBLANK, "Remove Empty Lines (Containing Blank characters)"),
                C(IDM_EDIT_BLANKLINEABOVECURRENT, "Insert Blank Line Above Current"),
                C(IDM_EDIT_BLANKLINEBELOWCURRENT, "Insert Blank Line Below Current"),
                C(IDM_EDIT_SORTLINES_REVERSE_ORDER, "Reverse Line Order"), C(IDM_EDIT_SORTLINES_RANDOMLY, "Randomize Line Order"), SEP,
                C(IDM_EDIT_SORTLINES_LEXICOGRAPHIC_ASCENDING, "Sort Lines Lexicographically Ascending"),
                C(IDM_EDIT_SORTLINES_LEXICO_CASE_INSENS_ASCENDING, "Sort Lines Lex. Ascending Ignoring Case"),
                C(IDM_EDIT_SORTLINES_LOCALE_ASCENDING, "Sort Lines In Locale Order Ascending"),
                C(IDM_EDIT_SORTLINES_INTEGER_ASCENDING, "Sort Lines As Integers Ascending"),
                C(IDM_EDIT_SORTLINES_DECIMALCOMMA_ASCENDING, "Sort Lines As Decimals (Comma) Ascending"),
                C(IDM_EDIT_SORTLINES_DECIMALDOT_ASCENDING, "Sort Lines As Decimals (Dot) Ascending"),
                C(IDM_EDIT_SORTLINES_LENGTH_ASCENDING, "Sort Lines By Length Ascending"), SEP,
                C(IDM_EDIT_SORTLINES_LEXICOGRAPHIC_DESCENDING, "Sort Lines Lexicographically Descending"),
                C(IDM_EDIT_SORTLINES_LEXICO_CASE_INSENS_DESCENDING, "Sort Lines Lex. Descending Ignoring Case"),
                C(IDM_EDIT_SORTLINES_LOCALE_DESCENDING, "Sort Lines In Locale Order Descending"),
                C(IDM_EDIT_SORTLINES_INTEGER_DESCENDING, "Sort Lines As Integers Descending"),
                C(IDM_EDIT_SORTLINES_DECIMALCOMMA_DESCENDING, "Sort Lines As Decimals (Comma) Descending"),
                C(IDM_EDIT_SORTLINES_DECIMALDOT_DESCENDING, "Sort Lines As Decimals (Dot) Descending"),
                C(IDM_EDIT_SORTLINES_LENGTH_DESCENDING, "Sort Lines By Length Descending")}),
            S("edit-comment", "Co&mment/Uncomment", {
                C(IDM_EDIT_BLOCK_COMMENT, "Toggle Single Line Comment"), C(IDM_EDIT_BLOCK_COMMENT_SET, "Single Line Comment"),
                C(IDM_EDIT_BLOCK_UNCOMMENT, "Single Line Uncomment"), C(IDM_EDIT_STREAM_COMMENT, "Block Comment"),
                C(IDM_EDIT_STREAM_UNCOMMENT, "Block Uncomment")}),
            S("edit-eolConversion", "&EOL Conversion", {
                C(IDM_FORMAT_TODOS, "Windows (CR LF)"), C(IDM_FORMAT_TOUNIX, "Unix (LF)"), C(IDM_FORMAT_TOMAC, "Macintosh (CR)")}),
            S("edit-blankOperations", "&Blank Operations", {
                C(IDM_EDIT_TRIMTRAILING, "Trim Trailing Space"), C(IDM_EDIT_TRIMLINEHEAD, "Trim Leading Space"),
                C(IDM_EDIT_TRIM_BOTH, "Trim Leading and Trailing Space"), C(IDM_EDIT_EOL2WS, "EOL to Space"),
                C(IDM_EDIT_TRIMALL, "Trim both and EOL to Space"), SEP,
                C(IDM_EDIT_TAB2SW, "TAB to Space"), C(IDM_EDIT_SW2TAB_ALL, "Space to TAB (All)"),
                C(IDM_EDIT_SW2TAB_LEADING, "Space to TAB (Leading)")}),
            S("edit-onSelection", "&On Selection", {
                C(IDM_EDIT_OPENSELECTEDFILETOEDIT, "Open File"),
                C(IDM_EDIT_OPENSELECTEDFILEFOLDERINEXPLORER, "Open Containing Folder"), SEP,
                C(IDM_EDIT_SEARCHONINTERNET, "Search on Internet")}),
            SEP,
            S("edit-multiSelectALL", "Multi-select All", {
                C(IDM_EDIT_MULTISELECTALL, "Ignore Case && Whole Word"), C(IDM_EDIT_MULTISELECTALLMATCHCASE, "Match Case Only"),
                C(IDM_EDIT_MULTISELECTALLWHOLEWORD, "Match Whole Word Only"),
                C(IDM_EDIT_MULTISELECTALLMATCHCASEWHOLEWORD, "Match Case && Whole Word")}),
            S("edit-multiSelectNext", "Multi-select Next", {
                C(IDM_EDIT_MULTISELECTNEXT, "Ignore Case && Whole Word"), C(IDM_EDIT_MULTISELECTNEXTMATCHCASE, "Match Case Only"),
                C(IDM_EDIT_MULTISELECTNEXTWHOLEWORD, "Match Whole Word Only"),
                C(IDM_EDIT_MULTISELECTNEXTMATCHCASEWHOLEWORD, "Match Case && Whole Word")}),
            C(IDM_EDIT_MULTISELECTUNDO, "Undo the Latest Added Multi-Select"),
            C(IDM_EDIT_MULTISELECTSSKIP, "Skip Current && Go to Next Multi-select"), SEP,
            C(IDM_EDIT_COLUMNMODE, "Colum&n Editor..."), C(IDM_EDIT_CHAR_PANEL, "Character &Panel"),
            C(IDM_EDIT_CLIPBOARDHISTORY_PANEL, "Clipboard &History"), SEP,
            S("edit-readonlyInNotepad++", "Read-&Only in Notepad++", {
                C(IDM_EDIT_TOGGLEREADONLY, "Read-Only on Current Document"),
                C(IDM_EDIT_SETREADONLYFORALLDOCS, "Read-Only for All Documents"),
                C(IDM_EDIT_CLEARREADONLYFORALLDOCS, "Clear Read-Only for All Documents")})}},
        {"search", "&Search", {
            C(IDM_SEARCH_FIND, "&Find..."), C(IDM_SEARCH_FINDINFILES, "Find in Fi&les..."),
            C(IDM_SEARCH_FINDNEXT, "Find &Next"), C(IDM_SEARCH_FINDPREV, "Find &Previous"),
            C(IDM_SEARCH_SETANDFINDNEXT, "&Select and Find Next"), C(IDM_SEARCH_SETANDFINDPREV, "&Select and Find Previous"),
            C(IDM_SEARCH_VOLATILE_FINDNEXT, "Find (&Volatile) Next"), C(IDM_SEARCH_VOLATILE_FINDPREV, "Find (&Volatile) Previous"),
            C(IDM_SEARCH_REPLACE, "&Replace..."), C(IDM_SEARCH_FINDINCREMENT, "&Incremental Search"),
            C(IDM_FOCUS_ON_FOUND_RESULTS, "Search Results &Window"), C(IDM_SEARCH_GOTONEXTFOUND, "Next Search Resul&t"),
            C(IDM_SEARCH_GOTOPREVFOUND, "Previous Search Resul&t"), C(IDM_SEARCH_GOTOLINE, "&Go to..."),
            C(IDM_SEARCH_GOTOMATCHINGBRACE, "Go to &Matching Brace"),
            C(IDM_SEARCH_SELECTMATCHINGBRACES, "Select All In-betw&een {} [] or ()"), C(IDM_SEARCH_MARK, "Mar&k..."), SEP,
            S("search-markAll", "Style &All Occurrences of Token", {
                C(IDM_SEARCH_MARKALLEXT1, "Using 1st Style"), C(IDM_SEARCH_MARKALLEXT2, "Using 2nd Style"),
                C(IDM_SEARCH_MARKALLEXT3, "Using 3rd Style"), C(IDM_SEARCH_MARKALLEXT4, "Using 4th Style"),
                C(IDM_SEARCH_MARKALLEXT5, "Using 5th Style")}),
            S("search-markOne", "Style &One Token", {
                C(IDM_SEARCH_MARKONEEXT1, "Using 1st Style"), C(IDM_SEARCH_MARKONEEXT2, "Using 2nd Style"),
                C(IDM_SEARCH_MARKONEEXT3, "Using 3rd Style"), C(IDM_SEARCH_MARKONEEXT4, "Using 4th Style"),
                C(IDM_SEARCH_MARKONEEXT5, "Using 5th Style")}),
            S("search-unmarkAll", "Clear Style", {
                C(IDM_SEARCH_UNMARKALLEXT1, "Clear 1st Style"), C(IDM_SEARCH_UNMARKALLEXT2, "Clear 2nd Style"),
                C(IDM_SEARCH_UNMARKALLEXT3, "Clear 3rd Style"), C(IDM_SEARCH_UNMARKALLEXT4, "Clear 4th Style"),
                C(IDM_SEARCH_UNMARKALLEXT5, "Clear 5th Style"), C(IDM_SEARCH_CLEARALLMARKS, "Clear all Styles")}),
            S("search-jumpUp", "&Jump Up", {
                C(IDM_SEARCH_GOPREVMARKER1, "1st Style"), C(IDM_SEARCH_GOPREVMARKER2, "2nd Style"),
                C(IDM_SEARCH_GOPREVMARKER3, "3rd Style"), C(IDM_SEARCH_GOPREVMARKER4, "4th Style"),
                C(IDM_SEARCH_GOPREVMARKER5, "5th Style"), C(IDM_SEARCH_GOPREVMARKER_DEF, "Find Mark Style")}),
            S("search-jumpDown", "Jump &Down", {
                C(IDM_SEARCH_GONEXTMARKER1, "1st Style"), C(IDM_SEARCH_GONEXTMARKER2, "2nd Style"),
                C(IDM_SEARCH_GONEXTMARKER3, "3rd Style"), C(IDM_SEARCH_GONEXTMARKER4, "4th Style"),
                C(IDM_SEARCH_GONEXTMARKER5, "5th Style"), C(IDM_SEARCH_GONEXTMARKER_DEF, "Find Mark Style")}),
            S("search-copyStyledText", "&Copy Styled Text", {
                C(IDM_SEARCH_STYLE1TOCLIP, "1st Style"), C(IDM_SEARCH_STYLE2TOCLIP, "2nd Style"),
                C(IDM_SEARCH_STYLE3TOCLIP, "3rd Style"), C(IDM_SEARCH_STYLE4TOCLIP, "4th Style"),
                C(IDM_SEARCH_STYLE5TOCLIP, "5th Style"), C(IDM_SEARCH_ALLSTYLESTOCLIP, "All Styles"),
                C(IDM_SEARCH_MARKEDTOCLIP, "Find Mark Style")}),
            SEP,
            S("search-bookmark", "&Bookmark", {
                C(IDM_SEARCH_TOGGLE_BOOKMARK, "Toggle Bookmark"), C(IDM_SEARCH_NEXT_BOOKMARK, "Next Bookmark"),
                C(IDM_SEARCH_PREV_BOOKMARK, "Previous Bookmark"), C(IDM_SEARCH_CLEAR_BOOKMARKS, "Clear All Bookmarks"),
                C(IDM_SEARCH_CUTMARKEDLINES, "Cut Bookmarked Lines"), C(IDM_SEARCH_COPYMARKEDLINES, "Copy Bookmarked Lines"),
                C(IDM_SEARCH_PASTEMARKEDLINES, "Paste to (Replace) Bookmarked Lines"),
                C(IDM_SEARCH_DELETEMARKEDLINES, "Remove Bookmarked Lines"),
                C(IDM_SEARCH_DELETEUNMARKEDLINES, "Remove Non-Bookmarked Lines"), C(IDM_SEARCH_INVERSEMARKS, "Inverse Bookmarks")})}},
        {"view", "&View", {
            C(IDM_VIEW_ALWAYSONTOP, "Always on &Top"), C(IDM_VIEW_FULLSCREENTOGGLE, "To&ggle Full Screen Mode"),
            C(IDM_VIEW_POSTIT, "&Post-It"), SEP,
            S("view-showSymbol", "Show S&ymbol", {
                C(IDM_VIEW_TAB_SPACE, "Show Space and Tab"), C(IDM_VIEW_EOL, "Show End of Line"),
                C(IDM_VIEW_ALL_CHARACTERS, "Show All Characters"), SEP,
                C(IDM_VIEW_INDENT_GUIDE, "Show Indent Guide"), C(IDM_VIEW_WRAP_SYMBOL, "Show Wrap Symbol")}),
            S("view-zoom", "&Zoom", {C(IDM_VIEW_ZOOMIN, "Zoom &In"), C(IDM_VIEW_ZOOMOUT, "Zoom &Out"),
                                     C(IDM_VIEW_ZOOMRESTORE, "&Restore Default Zoom")}),
            S("view-moveCloneDocument", "&Move/Clone Current Document", {
                C(IDM_VIEW_GOTO_ANOTHER_VIEW, "&Move to Other View"), C(IDM_VIEW_CLONE_TO_ANOTHER_VIEW, "&Clone to Other View")}),
            S("view-tab", "Ta&b", {
                C(IDM_VIEW_TAB1, "1st Tab"), C(IDM_VIEW_TAB2, "2nd Tab"), C(IDM_VIEW_TAB3, "3rd Tab"), C(IDM_VIEW_TAB4, "4th Tab"),
                C(IDM_VIEW_TAB5, "5th Tab"), C(IDM_VIEW_TAB6, "6th Tab"), C(IDM_VIEW_TAB7, "7th Tab"), C(IDM_VIEW_TAB8, "8th Tab"),
                C(IDM_VIEW_TAB9, "9th Tab"), SEP,
                C(IDM_VIEW_TAB_START, "First Tab"), C(IDM_VIEW_TAB_END, "Last Tab"),
                C(IDM_VIEW_TAB_NEXT, "Next Tab"), C(IDM_VIEW_TAB_PREV, "Previous Tab"), SEP,
                C(IDM_VIEW_GOTO_START, "Move to Start"), C(IDM_VIEW_GOTO_END, "Move to End"),
                C(IDM_VIEW_TAB_MOVEFORWARD, "Move Tab Forward"), C(IDM_VIEW_TAB_MOVEBACKWARD, "Move Tab Backward")}),
            C(IDM_VIEW_WRAP, "&Word wrap"), C(IDM_VIEW_SWITCHTO_OTHER_VIEW, "Focus on &Another View"),
            C(IDM_VIEW_HIDELINES, "&Hide Lines"), SEP,
            C(IDM_VIEW_FOLDALL, "Fold All"), C(IDM_VIEW_UNFOLDALL, "Unfold All"),
            C(IDM_VIEW_FOLD_CURRENT, "Fold Current Level"), C(IDM_VIEW_UNFOLD_CURRENT, "Unfold Current Level"),
            S("view-collapseLevel", "Fold Level", {
                C(IDM_VIEW_FOLD_1, "1"), C(IDM_VIEW_FOLD_2, "2"), C(IDM_VIEW_FOLD_3, "3"), C(IDM_VIEW_FOLD_4, "4"),
                C(IDM_VIEW_FOLD_5, "5"), C(IDM_VIEW_FOLD_6, "6"), C(IDM_VIEW_FOLD_7, "7"), C(IDM_VIEW_FOLD_8, "8")}),
            S("view-uncollapseLevel", "Unfold Level", {
                C(IDM_VIEW_UNFOLD_1, "1"), C(IDM_VIEW_UNFOLD_2, "2"), C(IDM_VIEW_UNFOLD_3, "3"), C(IDM_VIEW_UNFOLD_4, "4"),
                C(IDM_VIEW_UNFOLD_5, "5"), C(IDM_VIEW_UNFOLD_6, "6"), C(IDM_VIEW_UNFOLD_7, "7"), C(IDM_VIEW_UNFOLD_8, "8")}),
            SEP, C(IDM_VIEW_SUMMARY, "&Summary..."), SEP,
            S("view-project", "Pro&ject Panels", {C(IDM_VIEW_PROJECT_PANEL_1, "Project Panel &1"), C(IDM_VIEW_PROJECT_PANEL_2, "Project Panel &2"),
                                                 C(IDM_VIEW_PROJECT_PANEL_3, "Project Panel &3")}),
            C(IDM_VIEW_FILEBROWSER, "Folder as Wor&kspace"), C(IDM_VIEW_DOC_MAP, "&Document Map"),
            C(IDM_VIEW_DOCLIST, "D&ocument List"), C(IDM_VIEW_FUNC_LIST, "Function &List"), SEP,
            C(IDM_VIEW_SYNSCROLLV, "Sy&nchronize Vertical Scrolling"), C(IDM_VIEW_SYNSCROLLH, "Syn&chronize Horizontal Scrolling"),
            SEP, C(IDM_EDIT_RTL, "T&ext Direction RTL"), C(IDM_EDIT_LTR, "Te&xt Direction LTR"), SEP,
            C(IDM_VIEW_MONITORING, "Monito&ring (tail -f)")}},
        {"encoding", "E&ncoding", {
            C(IDM_FORMAT_ANSI, "ANSI"), C(IDM_FORMAT_AS_UTF_8, "UTF-8"), C(IDM_FORMAT_UTF_8, "UTF-8-BOM"),
            C(IDM_FORMAT_UTF_16BE, "UTF-16 BE BOM"), C(IDM_FORMAT_UTF_16LE, "UTF-16 LE BOM"),
            S("encoding-characterSets", "Character Set", {
                S("encoding-arabic", "Arabic", {C(IDM_FORMAT_ISO_8859_6, "ISO 8859-6"), C(IDM_FORMAT_DOS_720, "OEM 720"),
                                               C(IDM_FORMAT_WIN_1256, "Windows-1256")}),
                S("encoding-baltic", "Baltic", {C(IDM_FORMAT_ISO_8859_4, "ISO 8859-4"), C(IDM_FORMAT_ISO_8859_13, "ISO 8859-13"),
                                               C(IDM_FORMAT_DOS_775, "OEM 775"), C(IDM_FORMAT_WIN_1257, "Windows-1257")}),
                S("encoding-celtic", "Celtic", {C(IDM_FORMAT_ISO_8859_14, "ISO 8859-14")}),
                S("encoding-cyrillic", "Cyrillic", {
                    C(IDM_FORMAT_ISO_8859_5, "ISO 8859-5"), C(IDM_FORMAT_KOI8R_CYRILLIC, "KOI8-R"),
                    C(IDM_FORMAT_KOI8U_CYRILLIC, "KOI8-U"), C(IDM_FORMAT_MAC_CYRILLIC, "Macintosh"),
                    C(IDM_FORMAT_DOS_855, "OEM 855"), C(IDM_FORMAT_DOS_866, "OEM 866"), C(IDM_FORMAT_WIN_1251, "Windows-1251")}),
                S("encoding-centralEuropean", "Central European", {C(IDM_FORMAT_DOS_852, "OEM 852"), C(IDM_FORMAT_WIN_1250, "Windows-1250")}),
                S("encoding-chinese", "Chinese", {C(IDM_FORMAT_BIG5, "Big5 (Traditional)"), C(IDM_FORMAT_GB2312, "GB2312 (Simplified)")}),
                S("encoding-easternEuropean", "Eastern European", {C(IDM_FORMAT_ISO_8859_2, "ISO 8859-2")}),
                S("encoding-greek", "Greek", {C(IDM_FORMAT_ISO_8859_7, "ISO 8859-7"), C(IDM_FORMAT_DOS_737, "OEM 737"),
                                             C(IDM_FORMAT_DOS_869, "OEM 869"), C(IDM_FORMAT_WIN_1253, "Windows-1253")}),
                S("encoding-hebrew", "Hebrew", {C(IDM_FORMAT_ISO_8859_8, "ISO 8859-8"), C(IDM_FORMAT_DOS_862, "OEM 862"),
                                               C(IDM_FORMAT_WIN_1255, "Windows-1255")}),
                S("encoding-japanese", "Japanese", {C(IDM_FORMAT_SHIFT_JIS, "Shift-JIS")}),
                S("encoding-korean", "Korean", {C(IDM_FORMAT_KOREAN_WIN, "Windows 949"), C(IDM_FORMAT_EUC_KR, "EUC-KR")}),
                S("encoding-northEuropean", "North European", {C(IDM_FORMAT_DOS_861, "OEM 861 : Icelandic"),
                                                               C(IDM_FORMAT_DOS_865, "OEM 865 : Nordic")}),
                S("encoding-thai", "Thai", {C(IDM_FORMAT_TIS_620, "TIS-620")}),
                S("encoding-turkish", "Turkish", {C(IDM_FORMAT_ISO_8859_3, "ISO 8859-3"), C(IDM_FORMAT_ISO_8859_9, "ISO 8859-9"),
                                                 C(IDM_FORMAT_DOS_857, "OEM 857"), C(IDM_FORMAT_WIN_1254, "Windows-1254")}),
                S("encoding-westernEuropean", "Western European", {
                    C(IDM_FORMAT_ISO_8859_1, "ISO 8859-1"), C(IDM_FORMAT_ISO_8859_15, "ISO 8859-15"),
                    C(IDM_FORMAT_DOS_850, "OEM 850"), C(IDM_FORMAT_DOS_858, "OEM 858"),
                    C(IDM_FORMAT_DOS_860, "OEM 860 : Portuguese"), C(IDM_FORMAT_DOS_863, "OEM 863 : French"),
                    C(IDM_FORMAT_DOS_437, "OEM-US : CP437"), C(IDM_FORMAT_WIN_1252, "Windows-1252")}),
                S("encoding-vietnamese", "Vietnamese", {C(IDM_FORMAT_WIN_1258, "Windows-1258")})}),
            SEP,
            C(IDM_FORMAT_CONV2_ANSI, "Convert to ANSI"), C(IDM_FORMAT_CONV2_AS_UTF_8, "Convert to UTF-8"),
            C(IDM_FORMAT_CONV2_UTF_8, "Convert to UTF-8-BOM"), C(IDM_FORMAT_CONV2_UTF_16BE, "Convert to UTF-16 BE BOM"),
            C(IDM_FORMAT_CONV2_UTF_16LE, "Convert to UTF-16 LE BOM")}},
        {"language", "&Language", {
            D("languages"), SEP,
            S("language-userDefinedLanguage", "User Defined Language", {
                C(IDM_LANG_USER_DLG, "Define your language..."), C(IDM_LANG_OPENUDLDIR, "Open User Defined Language folder..."),
                D("userLanguages")})}},
        {"settings", "Se&ttings", {
            C(IDM_SETTING_PREFERENCE, "Preferences..."), C(IDM_LANGSTYLE_CONFIG_DLG, "Style Configurator..."),
            C(IDM_SETTING_SHORTCUT_MAPPER, "Shortcut Mapper..."), SEP,
            S("settings-import", "Import", {C(IDM_SETTING_IMPORTSTYLETHEMES, "Import style theme(s)..."),
                                            C(kCmdImportConfig, "Import Notepad++ config.xml...")}),
            D("themes")}},
        {"tools", "T&ools", {
            S("tools-md5", "MD5", {C(IDM_TOOL_MD5_GENERATEINTOCLIPBOARD, "Generate from selection into clipboard")}),
            S("tools-sha1", "SHA-1", {C(IDM_TOOL_SHA1_GENERATEINTOCLIPBOARD, "Generate from selection into clipboard")}),
            S("tools-sha256", "SHA-256", {C(IDM_TOOL_SHA256_GENERATEINTOCLIPBOARD, "Generate from selection into clipboard")}),
            S("tools-sha512", "SHA-512", {C(IDM_TOOL_SHA512_GENERATEINTOCLIPBOARD, "Generate from selection into clipboard")})}},
        {"macro", "&Macro", {
            C(IDM_MACRO_STARTRECORDINGMACRO, "Start Re&cording"), C(IDM_MACRO_STOPRECORDINGMACRO, "S&top Recording"),
            C(IDM_MACRO_PLAYBACKRECORDEDMACRO, "&Playback"), C(IDM_MACRO_SAVECURRENTMACRO, "&Save Current Recorded Macro..."),
            C(IDM_MACRO_RUNMULTIMACRODLG, "&Run a Macro Multiple Times..."), D("macros"), SEP,
            C(IDM_SETTING_SHORTCUT_MAPPER_MACRO, "Modify Shortcut/Delete Macro...")}},
        {"run", "&Run", {
            C(IDM_EXECUTE, "&Run..."), D("runCommands"), SEP,
            C(IDM_SETTING_SHORTCUT_MAPPER_RUN, "Modify Shortcut/Delete Command...")}},
        {"?", "&?", {
            C(IDM_CMDLINEARGUMENTS, "Command Line Arguments..."), SEP,
            C(IDM_HOMESWEETHOME, "Notepad++ Home"), C(IDM_PROJECTPAGE, "Notepad++ Project Page"),
            C(IDM_ONLINEDOCUMENT, "Notepad++ Online User Manual"), C(IDM_FORUM, "Notepad++ Community (Forum)"), SEP,
            C(IDM_DEBUGINFO, "Debug Info..."), C(IDM_ABOUT, "About Notepad++")}},
    };
}

GMenu* newSection(GMenu* parent)
{
    GMenu* s = g_menu_new();
    g_menu_append_section(parent, nullptr, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

struct Builder {
    const core::Translator& tr;
    const CommandTable& cmds;
    DynamicMenus& dyn;

    // Appends nodes to `parent`, splitting sections at separators. Returns
    // whether anything visible was added.
    bool fill(GMenu* parent, const std::vector<Node>& nodes)
    {
        bool any = false;
        GMenu* sec = newSection(parent);
        for (const Node& n : nodes) {
            switch (n.kind) {
            case Node::Sep:
                sec = newSection(parent);
                break;
            case Node::Cmd:
                if (cmds.has(n.id)) {
                    appendItem(sec, core::Translator::toMnemonic(label(tr, n)), CommandTable::detailedName(n.id));
                    any = true;
                }
                break;
            case Node::Sub: {
                GMenu* sub = g_menu_new();
                if (fill(sub, n.kids)) {
                    g_menu_append_submenu(sec, core::Translator::toMnemonic(tr.subMenu(n.key, n.text)).c_str(), G_MENU_MODEL(sub));
                    any = true;
                }
                g_object_unref(sub);
                break;
            }
            case Node::Dyn: {
                GMenu* d = newSection(parent);
                const std::string name = n.key;
                GMenu** slot = name == "recent" ? &dyn.recent : name == "themes" ? &dyn.themes
                             : name == "languages" ? &dyn.languages : name == "userLanguages" ? &dyn.userLanguages
                             : name == "macros" ? &dyn.macros : &dyn.runCommands;
                *slot = d;
                sec = newSection(parent); // items after a dynamic part get their own section
                any = true;
                break;
            }
            }
        }
        return any;
    }
};

} // namespace

void appendItem(GMenu* m, const std::string& label, const std::string& detailedAction, const char* target)
{
    GMenuItem* it = g_menu_item_new(label.c_str(), nullptr);
    if (target)
        g_menu_item_set_action_and_target_value(it, detailedAction.c_str(), g_variant_new_string(target));
    else
        g_menu_item_set_detailed_action(it, detailedAction.c_str());
    g_menu_append_item(m, it);
    g_object_unref(it);
}

GMenu* buildMenuBar(const core::Translator& tr, const CommandTable& cmds, DynamicMenus& dyn)
{
    dyn = {};
    GMenu* bar = g_menu_new();
    Builder b{tr, cmds, dyn};
    for (const Top& top : mainMenu()) {
        GMenu* m = g_menu_new();
        if (b.fill(m, top.items))
            g_menu_append_submenu(bar, core::Translator::toMnemonic(tr.menu(top.menuId, top.text)).c_str(), G_MENU_MODEL(m));
        g_object_unref(m);
    }
    return bar;
}

std::map<int, CommandInfo> commandInfos(const core::Translator& tr)
{
    std::map<int, CommandInfo> out;
    std::function<void(const std::vector<Node>&, const std::string&)> walk = [&](const std::vector<Node>& nodes, const std::string& cat) {
        for (const Node& n : nodes) {
            if (n.kind == Node::Cmd)
                out.try_emplace(n.id, CommandInfo{core::Translator::plain(label(tr, n)), cat});
            else if (n.kind == Node::Sub)
                walk(n.kids, cat);
        }
    };
    for (const Top& top : mainMenu())
        walk(top.items, core::Translator::plain(tr.menu(top.menuId, top.text)));
    return out;
}

GMenu* buildTabMenu(const core::Translator& tr, const CommandTable& cmds)
{
    GMenu* m = g_menu_new();
    DynamicMenus unused;
    Builder b{tr, cmds, unused};
    b.fill(m, {C(IDM_FILE_CLOSE, "Close"), C(IDM_FILE_CLOSEALL_BUT_CURRENT, "Close All BUT This"),
               C(IDM_FILE_CLOSEALL_TOLEFT, "Close All to the Left"), C(IDM_FILE_CLOSEALL_TORIGHT, "Close All to the Right"),
               C(IDM_FILE_CLOSEALL_UNCHANGED, "Close All Unchanged"), SEP,
               C(IDM_FILE_SAVE, "Save"), C(IDM_FILE_SAVEAS, "Save As..."), C(IDM_FILE_RENAME, "Rename"),
               P(IDM_FILE_DELETE, "Move to Trash"), C(IDM_FILE_RELOAD, "Reload"), C(IDM_FILE_PRINT, "Print"), SEP,
               P(IDM_FILE_OPEN_FOLDER, "Open Containing Folder"), P(IDM_FILE_OPEN_CMD, "Open in Terminal"), SEP,
               C(IDM_EDIT_TOGGLEREADONLY, "Read-Only"), SEP,
               C(IDM_EDIT_FULLPATHTOCLIP, "Copy Full File Path"), C(IDM_EDIT_FILENAMETOCLIP, "Copy Filename"),
               C(IDM_EDIT_CURRENTDIRTOCLIP, "Copy Current Dir. Path"), SEP,
               C(IDM_VIEW_GOTO_ANOTHER_VIEW, "Move to Other View"), C(IDM_VIEW_CLONE_TO_ANOTHER_VIEW, "Clone to Other View")});
    return m;
}

} // namespace npp::ui
