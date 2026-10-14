// Phase 3 UI tests on a live MainWindow: menu commands by upstream ID,
// Find / Replace / Find in Files / Mark through FindDialog, the search
// results panel, macro steps in upstream's format, nativeLang switching and
// shortcuts.xml.

#include "core/LanguageCatalog.h"
#include "core/Workspace.h"
#include "platform/FileIO.h"
#include "platform/Paths.h"
#include "ui/MainWindow.h"

#include "FindReplaceDlg_rc.h"
#include "menuCmdID.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace npp;
using Answer = ui::MainWindow::Answer;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

namespace {

fs::path g_dir;

void writeText(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string readText(const fs::path& p)
{
    std::string s;
    platform::readFile(p, s, nullptr);
    return s;
}

void pump()
{
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}


template <class F>
bool waitFor(F cond, int ms = 5000)
{
    const gint64 end = g_get_monotonic_time() + ms * 1000;
    while (!cond()) {
        if (g_get_monotonic_time() > end)
            return false;
        pump();
        g_usleep(20000);
    }
    return true;
}

struct Prompter {
    std::deque<Answer> answers;
    std::vector<std::string> asked;
    ui::MainWindow::PromptHandler handler()
    {
        return [this](const std::string& primary, const std::string&, bool) {
            asked.push_back(primary);
            if (answers.empty())
                return Answer::No;
            const Answer a = answers.front();
            answers.pop_front();
            return a;
        };
    }
};

std::unique_ptr<ui::MainWindow> makeWindow(GtkApplication* app, core::LanguageCatalog& langs, Prompter& p)
{
    auto w = std::make_unique<ui::MainWindow>(app, langs, false);
    w->setPromptHandler(p.handler());
    w->present();
    pump();
    return w;
}

void setSel(ui::EditorView& v, sptr_t a, sptr_t b)
{
    v.execute(SCI_SETSEL, static_cast<uptr_t>(a), b);
}

void testEditCommands(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    w->newDocument();
    ui::EditorView& v = *w->activeView();

    // Commands exist for upstream IDs, toggles report their state.
    for (int id : {IDM_FILE_NEW, IDM_EDIT_UPPERCASE, IDM_EDIT_SORTLINES_INTEGER_ASCENDING, IDM_SEARCH_FIND, IDM_SEARCH_MARK,
                   IDM_VIEW_WRAP, IDM_FORMAT_CONV2_UTF_8, IDM_FORMAT_WIN_1252, IDM_MACRO_STARTRECORDINGMACRO})
        CHECK(w->hasCommand(id) || id == IDM_MACRO_STARTRECORDINGMACRO);
    const bool wrap = w->commandChecked(IDM_VIEW_WRAP);
    CHECK(w->runCommand(IDM_VIEW_WRAP));
    CHECK(w->commandChecked(IDM_VIEW_WRAP) != wrap);
    CHECK((v.execute(SCI_GETWRAPMODE) != SC_WRAP_NONE) != wrap);
    w->runCommand(IDM_VIEW_WRAP);

    v.setText("banana\napple\ncherry\n");
    CHECK(w->runCommand(IDM_EDIT_SORTLINES_LEXICOGRAPHIC_ASCENDING));
    CHECK(v.text() == "\napple\nbanana\ncherry");
    setSel(v, 1, 6);
    CHECK(w->runCommand(IDM_EDIT_UPPERCASE));
    CHECK(v.text() == "\nAPPLE\nbanana\ncherry");
    CHECK(w->runCommand(IDM_EDIT_UNDO));
    CHECK(v.text() == "\napple\nbanana\ncherry");

    CHECK(w->runCommand(IDM_FORMAT_TODOS));
    CHECK(v.execute(SCI_GETEOLMODE) == SC_EOL_CRLF && v.text() == "\r\napple\r\nbanana\r\ncherry");
    CHECK(w->commandChecked(IDM_FORMAT_TODOS) && !w->commandChecked(IDM_FORMAT_TOUNIX));
    CHECK(w->runCommand(IDM_FORMAT_TOUNIX));

    // Encoding > Character Set and UTF-8 read the file again in that encoding.
    {
        const fs::path enc = g_dir / "enc.txt";
        writeText(enc, "caf\xC3\xA9\n");
        CHECK(w->openFiles({enc}) == 1);
        ui::EditorView& ev = *w->activeView();
        CHECK(ev.text() == "caf\xC3\xA9\n" && w->commandChecked(IDM_FORMAT_AS_UTF_8));
        CHECK(w->runCommand(IDM_FORMAT_WIN_1252));
        CHECK(ev.text() == "caf\xC3\x83\xC2\xA9\n"); // "cafÃ©"
        CHECK(w->commandChecked(IDM_FORMAT_WIN_1252) && !w->commandChecked(IDM_FORMAT_AS_UTF_8));
        CHECK(!w->isModified(*w->activeBuffer()));
        CHECK(w->runCommand(IDM_FORMAT_AS_UTF_8));
        CHECK(ev.text() == "caf\xC3\xA9\n" && w->commandChecked(IDM_FORMAT_AS_UTF_8));
        // Convert to: the text stays, the bytes on disk change on save.
        CHECK(w->runCommand(IDM_FORMAT_CONV2_UTF_16LE));
        CHECK(w->isModified(*w->activeBuffer()) && w->commandChecked(IDM_FORMAT_UTF_16LE));
        CHECK(w->runCommand(IDM_FILE_SAVE));
        CHECK(readText(enc) == std::string("\xFF\xFE" "c\0a\0f\0\xE9\0\n\0", 12));
        CHECK(w->runCommand(IDM_FILE_CLOSE));
    }

    // Comments follow the document's language.
    const fs::path cpp = g_dir / "c.cpp";
    writeText(cpp, "int a;\nint b;\n");
    CHECK(w->openFiles({cpp}) == 1);
    ui::EditorView& c = *w->activeView();
    setSel(c, 0, c.length());
    CHECK(w->runCommand(IDM_EDIT_BLOCK_COMMENT));
    CHECK(c.text() == "// int a;\n// int b;\n");
    CHECK(w->runCommand(IDM_EDIT_BLOCK_UNCOMMENT));
    CHECK(c.text() == "int a;\nint b;\n");

    // Read-only documents are not edited by Edit operations.
    CHECK(w->runCommand(IDM_EDIT_TOGGLEREADONLY));
    setSel(c, 0, 3);
    w->runCommand(IDM_EDIT_UPPERCASE);
    CHECK(c.text() == "int a;\nint b;\n");
    CHECK(w->runCommand(IDM_EDIT_TOGGLEREADONLY));

    // Bookmarks and Style tokens.
    c.execute(SCI_GOTOLINE, 1);
    CHECK(w->runCommand(IDM_SEARCH_TOGGLE_BOOKMARK));
    CHECK(w->runCommand(IDM_SEARCH_INVERSEMARKS));
    CHECK(c.execute(SCI_MARKERNEXT, 0, 1 << 24) == 0);
    c.execute(SCI_GOTOPOS, 1); // inside "int"
    CHECK(w->runCommand(IDM_SEARCH_MARKALLEXT1));
    const int ind = core::markStyleIndicator(1);
    CHECK(c.execute(SCI_INDICATORVALUEAT, ind, 0) && c.execute(SCI_INDICATORVALUEAT, ind, 7));
    CHECK(w->runCommand(IDM_SEARCH_CLEARALLMARKS));
    CHECK(!c.execute(SCI_INDICATORVALUEAT, ind, 0));

    // Printing: exported to PDF, one page per screenful of lines and more.
    {
        std::string text;
        for (int i = 0; i < 200; ++i)
            text += "line " + std::to_string(i) + "\n";
        c.setText(text);
        const fs::path pdf = g_dir / "print.pdf";
        std::string err;
        int pages = 0;
        CHECK(w->print(false, pdf, &err, &pages));
        CHECK(readText(pdf).rfind("%PDF", 0) == 0);
        CHECK(pages >= 3);
        // The selection only: one page.
        c.execute(SCI_SETSEL, 0, 20);
        CHECK(w->print(false, pdf, &err, &pages) && pages == 1);
        c.setText("int a;\nint b;\n");
    }

    // Tab commands.
    CHECK(w->runCommand(IDM_VIEW_TAB1));
    CHECK(w->activeBuffer()->isUntitled());
    CHECK(w->runCommand(IDM_VIEW_TAB_END));
    CHECK(w->activeBuffer()->path == cpp);
    p.answers = {Answer::No};
    CHECK(w->runCommand(IDM_FILE_CLOSEALL_BUT_CURRENT));
    CHECK(w->documentCount() == 1 && w->activeBuffer()->path == cpp);
}

void testFindDialog(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    const fs::path a = g_dir / "find" / "a.txt", b = g_dir / "find" / "sub" / "b.txt", skip = g_dir / "find" / "c.bin";
    writeText(a, "foo one\nnothing\nfoo foo\n");
    writeText(b, "x foo\n");
    writeText(skip, "foo");
    CHECK(w->openFiles({a}) == 1);
    ui::FindDialog& d = w->findDialog();
    ui::EditorView& v = *w->activeView();

    core::FindOptions o;
    o.what = "foo";
    v.execute(SCI_GOTOPOS, 0);
    CHECK(d.doFindNext(o) == core::FindStatus::Found && v.selectedText() == "foo");
    CHECK(d.doCount(o) == 3);
    CHECK(d.status().find("Count: 3 matches") == 0);
    o.what = "zzz";
    CHECK(d.doFindNext(o) == core::FindStatus::NotFound);
    CHECK(d.status().find("Can't find the text \"zzz\"") != std::string::npos);
    o.what = "(";
    o.mode = core::SearchMode::Regex;
    CHECK(d.doCount(o) == -1 && d.status().find("Invalid regular expression") != std::string::npos);

    // Mark with bookmarks, then clear.
    o = {};
    o.what = "foo";
    o.bookmarkLine = true;
    o.purge = true;
    CHECK(d.doMarkAll(o) == 3);
    CHECK(v.execute(SCI_INDICATORVALUEAT, core::kFoundIndicator, 0) != 0);
    CHECK(v.execute(SCI_MARKERNEXT, 1, 1 << 24) == 2); // lines 0 and 2 bookmarked, not 1
    d.doClearMarks(o);
    CHECK(v.execute(SCI_INDICATORVALUEAT, core::kFoundIndicator, 0) == 0 && v.execute(SCI_MARKERNEXT, 0, 1 << 24) == -1);

    // Find All in the current document -> results panel.
    CHECK(d.doFindAllInCurrent(o) == 3);
    ui::SearchResults& r = w->searchResults();
    std::string txt = r.text();
    CHECK(txt.find("Search \"foo\" (3 hits in 1 file of 1 searched) [Normal]") == 0);
    CHECK(txt.find("  " + a.string() + " (3 hits)\n") != std::string::npos);
    CHECK(txt.find("\tLine 1: foo one\n") != std::string::npos);
    CHECK(txt.find("\tLine 3: foo foo") != std::string::npos); // one entry for two hits
    CHECK(gtk_widget_get_visible(r.widget()));

    // F4 opens results in turn.
    v.execute(SCI_GOTOPOS, 0);
    CHECK(r.next(true));
    CHECK(v.lineFromPos(v.currentPos()) == 0 && v.selectedText() == "foo");
    CHECK(r.next(true));
    CHECK(v.lineFromPos(v.currentPos()) == 2);

    // Find in Files: an open (modified) document is searched as in the editor.
    v.execute(SCI_DOCUMENTEND);
    v.execute(SCI_ADDTEXT, 4, reinterpret_cast<sptr_t>("foo\n"));
    o = {};
    o.what = "foo";
    o.directory = (g_dir / "find").string();
    o.filters = "*.txt";
    CHECK(d.doFindInFiles(o) == 5);
    txt = r.text();
    CHECK(txt.find("Search \"foo\" (5 hits in 2 files of 2 searched)") == 0);
    CHECK(txt.find(b.string() + " (1 hit)") != std::string::npos);
    CHECK(txt.find("c.bin") == std::string::npos);
    CHECK(txt.find("Search \"foo\" (3 hits in 1 file", 10) != std::string::npos); // the earlier search stays below

    // Replace in Files: files on disk, and the open document in the editor.
    p.answers = {Answer::Yes};
    o.with = "bar";
    CHECK(d.doReplaceInFiles(o) == 5);
    CHECK(readText(b) == "x bar\n");
    CHECK(v.text() == "bar one\nnothing\nbar bar\nbar\n");
    CHECK(readText(a) == "foo one\nnothing\nfoo foo\n"); // not saved behind the user's back
    CHECK(w->isModified(*w->activeBuffer()));

    // Replace, then Replace All in the current document (one undo step).
    v.setText("cat cat cat");
    o = {};
    o.what = "cat";
    o.with = "dog";
    v.execute(SCI_GOTOPOS, 0);
    CHECK(d.doFindNext(o) == core::FindStatus::Found);
    CHECK(d.doReplace(o) != core::FindStatus::NotFound);
    CHECK(v.text() == "dog cat cat" && d.status().find("1 occurrence was replaced") != std::string::npos);
    CHECK(d.doReplaceAll(o) == 2);
    CHECK(v.text() == "dog dog dog");
    v.execute(SCI_UNDO);
    CHECK(v.text() == "dog cat cat");

    // Macro playback of upstream's saved search steps (Replace All).
    d.execSaved(IDC_FRCOMMAND_INIT, 0, "");
    d.execSaved(IDFINDWHAT, 0, "c(a)t");
    d.execSaved(IDNORMAL, 2, "");
    d.execSaved(IDREPLACEWITH, 0, "b$1t");
    d.execSaved(IDC_FRCOMMAND_BOOLEANS, IDF_WRAP | IDF_WHICH_DIRECTION, "");
    d.execSaved(IDC_FRCOMMAND_EXEC, IDREPLACEALL, "");
    CHECK(v.text() == "dog bat bat");

    // Incremental search: as you type, wrapping, highlight all with a count.
    v.setText("Alpha beta alpha");
    v.execute(SCI_GOTOPOS, 0);
    ui::FindBar& inc = w->incrementalSearch();
    CHECK(w->runCommand(IDM_SEARCH_FINDINCREMENT));
    CHECK(inc.isVisible());
    inc.setText("alpha");
    CHECK(v.execute(SCI_GETSELECTIONSTART) == 0 && v.selectedText() == "Alpha");
    inc.search(false, true);
    CHECK(v.execute(SCI_GETSELECTIONSTART) == 11);
    inc.search(false, true);
    CHECK(v.execute(SCI_GETSELECTIONSTART) == 0 && inc.status().find("continued from top") != std::string::npos);
    inc.setMatchCase(true);
    CHECK(v.execute(SCI_GETSELECTIONSTART) == 11);
    inc.setHighlightAll(true);
    CHECK(v.execute(SCI_INDICATORVALUEAT, core::kIncrementalIndicator, 11) != 0 && v.execute(SCI_INDICATORVALUEAT, core::kIncrementalIndicator, 0) == 0);
    inc.setText("zzz");
    CHECK(inc.status() == "Phrase not found");
    inc.hide();
    CHECK(!inc.isVisible() && v.execute(SCI_INDICATORVALUEAT, core::kIncrementalIndicator, 11) == 0);
    v.setText("dog bat bat");

    // The dialog itself: fields -> options, Find Next from the window (F3).
    d.show(ui::FindDialog::Tab::Replace);
    pump();
    CHECK(d.isVisible());
    d.setSearchText("bat");
    v.execute(SCI_GOTOPOS, 0);
    d.findNext(true);
    CHECK(v.selectedText() == "bat" && v.execute(SCI_GETSELECTIONSTART) == 4);
    CHECK(w->runCommand(IDM_SEARCH_FINDNEXT));
    CHECK(v.execute(SCI_GETSELECTIONSTART) == 8);
    d.hide();
    CHECK(!d.isVisible());
    CHECK(!w->settings().find.what.empty() && w->settings().find.what.front() == "bat");
}

void testLocalizationAndShortcuts(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    w->setLocalization("french.xml");
    CHECK(w->translator().fileName() == "french.xml");
    CHECK(w->settings().localization == "french.xml");
    CHECK(w->translator().command(IDM_FILE_NEW, "?") != "&New");
    w->setLocalization("");
    CHECK(w->settings().localization.empty());

    // Default upstream keys...
    gchar** accels = gtk_application_get_accels_for_action(app, ui::CommandTable::detailedName(IDM_FILE_NEW).c_str());
    CHECK(accels && accels[0] && std::string(accels[0]) == "<Primary>n");
    g_strfreev(accels);
    // ... and shortcuts.xml on top of them.
    writeText(platform::userConfigDir() / "shortcuts.xml",
              "<NotepadPlus><InternalCommands>"
              "<Shortcut id=\"41001\" Ctrl=\"yes\" Alt=\"yes\" Shift=\"no\" Key=\"78\" />"
              "</InternalCommands></NotepadPlus>");
    w->reloadShortcuts();
    accels = gtk_application_get_accels_for_action(app, ui::CommandTable::detailedName(IDM_FILE_NEW).c_str());
    CHECK(accels && accels[0] && std::string(accels[0]) == "<Primary><Alt>n");
    g_strfreev(accels);
    fs::remove(platform::userConfigDir() / "shortcuts.xml");
    w->reloadShortcuts();
}

void testMacrosAndRun(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    w->newDocument();
    ui::EditorView& v = *w->activeView();
    CHECK(!w->isEnabled(IDM_MACRO_PLAYBACKRECORDEDMACRO));

    // Record typing, a menu command and a search, as upstream does.
    CHECK(w->runCommand(IDM_MACRO_STARTRECORDINGMACRO));
    CHECK(w->isRecording() && !w->isEnabled(IDM_MACRO_STARTRECORDINGMACRO));
    v.execute(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>("abc x"));
    v.execute(SCI_NEWLINE);
    v.execute(SCI_LINEUP);
    v.execute(SCI_HOME);
    v.execute(SCI_LINEENDEXTEND);
    CHECK(w->runCommand(IDM_EDIT_UPPERCASE));
    v.execute(SCI_LINEDOWN);
    ui::FindDialog& d = w->findDialog();
    d.setSearchText("ABC");
    d.click(IDCCOUNTALL);
    CHECK(w->runCommand(IDM_MACRO_STOPRECORDINGMACRO));
    CHECK(!w->isRecording());
    CHECK(v.text() == "ABC X\n");
    const core::Macro m = w->recordedMacro();
    CHECK(!m.empty() && m.front().type == core::MacroStep::UseSParam && m.front().sParam == "abc x");
    const auto has = [&](auto pred) { return std::any_of(m.begin(), m.end(), pred); };
    CHECK(has([](const core::MacroStep& s) { return s.type == core::MacroStep::MenuCommand && s.wParam == IDM_EDIT_UPPERCASE; }));
    CHECK(!has([](const core::MacroStep& s) { return s.message == SCI_REPLACETARGET; })); // not twice
    CHECK(has([](const core::MacroStep& s) { return s.type == core::MacroStep::SavedSearch && s.message == IDFINDWHAT && s.sParam == "ABC"; }));
    CHECK(has([](const core::MacroStep& s) {
        return s.type == core::MacroStep::SavedSearch && s.message == IDC_FRCOMMAND_EXEC && s.lParam == IDCCOUNTALL;
    }));

    // Playback: once, then several times as one undo step.
    CHECK(w->isEnabled(IDM_MACRO_PLAYBACKRECORDEDMACRO));
    CHECK(w->runCommand(IDM_MACRO_PLAYBACKRECORDEDMACRO));
    CHECK(v.text() == "ABC X\nABC X\n");
    w->playMacro(m, 2);
    CHECK(v.text() == "ABC X\nABC X\nABC X\nABC X\n");
    v.execute(SCI_UNDO);
    CHECK(v.text() == "ABC X\nABC X\n");

    // Until the end of the file: a macro that uppercases a line and moves down.
    v.setText("a\nb\nc\n");
    core::Macro down = {core::MacroStep{core::MacroStep::UseLParam, SCI_HOME, 0, 0, {}},
                        core::MacroStep{core::MacroStep::UseLParam, SCI_LINEENDEXTEND, 0, 0, {}},
                        core::MacroStep::command(IDM_EDIT_UPPERCASE),
                        core::MacroStep{core::MacroStep::UseLParam, SCI_LINEDOWN, 0, 0, {}}};
    w->playMacro(down, -1);
    CHECK(v.text() == "A\nB\nC\n");

    // Saved macros go to shortcuts.xml in upstream's format and come back.
    w->addSavedMacro({"Upper line", core::KeyCombo{true, true, false, 'U'}, {}, down});
    const std::string xml = readText(platform::userConfigDir() / "shortcuts.xml");
    CHECK(xml.find("<Macro name=\"Upper line\" Ctrl=\"yes\" Alt=\"yes\" Shift=\"no\" Key=\"85\"") != std::string::npos);
    CHECK(xml.find("<Action type=\"2\" message=\"0\" wParam=\"" + std::to_string(IDM_EDIT_UPPERCASE) + "\"") != std::string::npos);
    w->reloadShortcuts();
    CHECK(w->savedMacros().size() == 1 && w->savedMacros()[0].steps == down);
    gchar** accels = gtk_application_get_accels_for_action(app, "win.run-macro::0");
    CHECK(accels && accels[0] && std::string(accels[0]) == "<Primary><Alt>u");
    g_strfreev(accels);
    v.setText("q\n");
    g_action_group_activate_action(G_ACTION_GROUP(w->window()), "run-macro", g_variant_new_string("0"));
    CHECK(v.text() == "Q\n");

    // Run: upstream variables.
    const fs::path f = g_dir / "run" / "doc.txt";
    writeText(f, "hello world\nsecond");
    CHECK(w->openFiles({f}) == 1);
    ui::EditorView& rv = *w->activeView();
    rv.execute(SCI_GOTOPOS, 2);
    CHECK(w->expandRunVariables("$(FULL_CURRENT_PATH)|$(CURRENT_DIRECTORY)|$(FILE_NAME)|$(NAME_PART)|$(EXT_PART)") ==
          f.string() + "|" + f.parent_path().string() + "|doc.txt|doc|.txt");
    CHECK(w->expandRunVariables("$(CURRENT_WORD) $(CURRENT_LINE):$(CURRENT_COLUMN) [$(CURRENT_LINESTR)] $(NOPE)") ==
          "hello 1:3 [hello world] $(NOPE)");
    g_setenv("NPP_TEST_VAR", "v", TRUE);
    CHECK(w->expandRunVariables("$(SYS.NPP_TEST_VAR)") == "v");
    const fs::path marker = g_dir / "run" / "ran.txt";
    std::string err;
    CHECK(w->runUserCommand("touch \"$(CURRENT_DIRECTORY)/ran.txt\"", &err));
    for (int i = 0; i < 100 && !fs::exists(marker); ++i)
        g_usleep(20000);
    CHECK(fs::exists(marker));
    CHECK(!w->runUserCommand("\"unterminated", &err) && !err.empty());
    fs::remove(platform::userConfigDir() / "shortcuts.xml");
}

void testPreferences(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    platform::Settings& st = w->settings();
    st.newDocEol = SC_EOL_CRLF;
    st.newDocEncoding = 1;
    st.newDocLanguage = "python";
    w->newDocument();
    ui::EditorView& v = *w->activeView();
    CHECK(v.execute(SCI_GETEOLMODE) == SC_EOL_CRLF);
    CHECK(w->activeBuffer()->encoding.bom == platform::Bom::Utf8);
    CHECK(w->activeBuffer()->language && w->activeBuffer()->language->name == "python");

    st.foldStyle = 4; // none
    st.showLineNumbers = false;
    st.showBookmarkMargin = false;
    st.edgeMode = 1;
    st.edgeColumns = "72";
    st.virtualSpace = true;
    w->applySettings();
    // Margins as upstream: 0 line numbers, 1 bookmarks, 2 change history, 3 folding.
    CHECK(v.execute(SCI_GETMARGINWIDTHN, 3) == 0 && v.execute(SCI_GETMARGINWIDTHN, 0) == 0 && v.execute(SCI_GETMARGINWIDTHN, 1) == 0);
    CHECK(v.execute(SCI_GETEDGEMODE) == EDGE_LINE && v.execute(SCI_GETEDGECOLUMN) == 72);
    CHECK(v.execute(SCI_GETVIRTUALSPACEOPTIONS) & SCVS_USERACCESSIBLE);
    st.foldStyle = 1;
    st.showLineNumbers = true;
    w->applySettings();
    CHECK(v.execute(SCI_GETMARGINWIDTHN, 2) > 0 && v.execute(SCI_GETMARGINWIDTHN, 0) > 0);
    CHECK(v.execute(SCI_MARKERSYMBOLDEFINED, SC_MARKNUM_FOLDER) == SC_MARK_ARROW);
    st = platform::Settings{};
}

void testUserDefinedLanguages(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    CHECK(w->userLanguages().size() == 4); // Markdown and Device Tree, light and dark
    const fs::path md = g_dir / "readme.md";
    writeText(md, "# Title\nplain **bold** text\n");
    CHECK(w->openFiles({md}) == 1);
    core::Buffer* b = w->activeBuffer();
    CHECK(b->userLanguage && b->userLanguage->name == "Markdown (preinstalled)"); // the light variant for the default theme
    ui::EditorView& v = *w->activeView();
    char lexer[32] = {};
    v.execute(SCI_GETLEXERLANGUAGE, 0, reinterpret_cast<sptr_t>(lexer));
    CHECK(std::string(lexer) == "user");
    v.execute(SCI_COLOURISE, 0, -1);
    CHECK(v.execute(SCI_GETSTYLEAT, 2) == SCE_USER_STYLE_COMMENTLINE);
    const sptr_t bold = static_cast<sptr_t>(v.text().find("**bold**")) + 3;
    CHECK(v.execute(SCI_GETSTYLEAT, static_cast<uptr_t>(bold)) >= SCE_USER_STYLE_DELIMITER1);
    CHECK(w->commandChecked(IDM_LANG_USER_DLG) == false);
    // Choosing a built-in language clears it; the Language menu brings it back.
    g_action_group_change_action_state(G_ACTION_GROUP(w->window()), "language", g_variant_new_string("cpp"));
    CHECK(!b->userLanguage && b->language->name == "cpp");
    g_action_group_change_action_state(G_ACTION_GROUP(w->window()), "language", g_variant_new_string("udl:Markdown (preinstalled)"));
    CHECK(b->userLanguage != nullptr);
    // Sessions store the UDL by name.
    const core::Session s = w->currentSession();
    bool found = false;
    for (const auto& f : s.views[0])
        if (f.filename == md.string())
            found = f.lang == "Markdown (preinstalled)";
    CHECK(found);
}

void testDeviceTree(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    const std::string text = "// SPDX-License-Identifier: GPL-2.0\n"
                             "/dts-v1/;\n"
                             "#include <dt-bindings/gpio/gpio.h>\n"
                             "/* board */\n"
                             "/ {\n"
                             "\tmodel = \"Board\";\n"
                             "\t#address-cells = <1>;\n"
                             "\tuart0: serial@1000 {\n"
                             "\t\treg = <0x1000 0x100>;\n"
                             "\t\tgpios = <&gpio 3 GPIO_ACTIVE_LOW>;\n"
                             "\t};\n"
                             "};\n";
    for (const char* name : {"board.dts", "soc.dtsi", "overlay.dtso"}) {
        const fs::path f = g_dir / "dt" / name;
        writeText(f, text);
        CHECK(w->openFiles({f}) == 1);
        const core::Buffer* b = w->activeBuffer();
        CHECK(b->userLanguage && b->userLanguage->name == "Device Tree (preinstalled)");
    }
    ui::EditorView& v = *w->activeView();
    v.execute(SCI_COLOURISE, 0, -1);
    auto styleAt = [&](const std::string& what, std::size_t offset = 0) {
        const std::size_t at = v.text().find(what);
        CHECK(at != std::string::npos);
        return static_cast<int>(v.execute(SCI_GETSTYLEAT, static_cast<uptr_t>(at + offset)));
    };
    CHECK(styleAt("/dts-v1/", 1) == SCE_USER_STYLE_KEYWORD1);
    CHECK(styleAt("#include", 1) == SCE_USER_STYLE_KEYWORD2);
    CHECK(styleAt("// SPDX", 3) == SCE_USER_STYLE_COMMENTLINE);
    CHECK(styleAt("/* board", 3) == SCE_USER_STYLE_COMMENT);
    CHECK(styleAt("model") == SCE_USER_STYLE_KEYWORD3);
    CHECK(styleAt("#address-cells", 9) == SCE_USER_STYLE_KEYWORD3);
    CHECK(styleAt("\"Board", 2) == SCE_USER_STYLE_DELIMITER1);
    CHECK(styleAt("0x1000", 3) == SCE_USER_STYLE_NUMBER);
    CHECK(styleAt("&gpio", 2) == SCE_USER_STYLE_KEYWORD5);
    CHECK(styleAt("GPIO_ACTIVE_LOW", 4) == SCE_USER_STYLE_KEYWORD4);
    CHECK(styleAt("= <1>") == SCE_USER_STYLE_OPERATOR);
    // Nodes fold, after line and block comments too.
    auto header = [&](int line) { return (v.execute(SCI_GETFOLDLEVEL, static_cast<uptr_t>(line)) & SC_FOLDLEVELHEADERFLAG) != 0; };
    CHECK(!header(3) && header(4) && !header(5) && header(7) && !header(8));
}

void testTabStripDoubleClick(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    platform::Settings& st = w->settings();
    GtkNotebook* nb = w->notebook(ui::MainWindow::kMainView);
    w->newDocument();
    // A double click at a point of the notebook's window (the allocations'
    // coordinates); true when it was handled.
    auto doubleClick = [&](int x, int y) {
        GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(nb));
        int ox = 0, oy = 0;
        gdk_window_get_origin(gw, &ox, &oy);
        GdkEvent* ev = gdk_event_new(GDK_2BUTTON_PRESS);
        ev->button.window = GDK_WINDOW(g_object_ref(gw));
        ev->button.button = 1;
        ev->button.x_root = ox + x;
        ev->button.y_root = oy + y;
        gdk_event_set_device(ev, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())));
        const bool handled = gtk_widget_event(GTK_WIDGET(nb), ev);
        gdk_event_free(ev);
        pump();
        return handled;
    };
    for (bool vertical : {false, true}) {
        st.tabVertical = vertical;
        w->applySettings();
        pump();
        const std::size_t before = w->tabCount(ui::MainWindow::kMainView);
        GtkWidget* lastLabel = gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, gtk_notebook_get_n_pages(nb) - 1));
        GtkAllocation l, n;
        gtk_widget_get_allocation(lastLabel, &l);
        gtk_widget_get_allocation(GTK_WIDGET(nb), &n);
        // On the last tab's label: not handled (nor closed, the option is off).
        CHECK(!st.tabDoubleClickClose && !doubleClick(l.x + l.width / 2, l.y + l.height / 2));
        CHECK(w->tabCount(ui::MainWindow::kMainView) == before);
        // In the editor: nothing either.
        CHECK(!doubleClick(n.x + n.width / 2, n.y + n.height / 2));
        // Past the last tab, on the strip: a new document, active.
        const int x = vertical ? l.x + l.width / 2 : l.x + l.width + 60;
        const int y = vertical ? l.y + l.height + 60 : l.y + l.height / 2;
        CHECK(ui::MainWindow::onEmptyTabStrip(nb, 0, 0) == false);
        CHECK(doubleClick(x, y));
        CHECK(w->tabCount(ui::MainWindow::kMainView) == before + 1);
        CHECK(w->activeBuffer() && w->activeBuffer()->isUntitled());
    }
    st.tabVertical = false;
    w->applySettings();
}

void testTabStripScroll(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    platform::Settings& st = w->settings();
    GtkNotebook* nb = w->notebook(ui::MainWindow::kMainView);
    for (int i = 0; i < 4; ++i)
        w->newDocument();
    pump();
    const int n = gtk_notebook_get_n_pages(nb);
    CHECK(n >= 4);
    // The notebook's tab-strip input window takes wheel events.
    bool masked = false;
    for (GList* l = gdk_window_peek_children(gtk_widget_get_window(GTK_WIDGET(nb))); l; l = l->next) {
        gpointer owner = nullptr;
        gdk_window_get_user_data(GDK_WINDOW(l->data), &owner);
        if (owner == nb && (gdk_window_get_events(GDK_WINDOW(l->data)) & GDK_SCROLL_MASK))
            masked = true;
    }
    CHECK(masked);
    // So do the tab labels and their close buttons, passing it to the notebook.
    GtkWidget* firstLabel = gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, 0));
    CHECK(gtk_widget_get_events(firstLabel) & GDK_SCROLL_MASK);
    // A wheel step at a point of the notebook's window; true when handled.
    auto wheel = [&](int x, int y, GdkScrollDirection dir, GdkModifierType mods = GdkModifierType(0), double dy = 0) {
        GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(nb));
        int ox = 0, oy = 0;
        gdk_window_get_origin(gw, &ox, &oy);
        GdkEvent* ev = gdk_event_new(GDK_SCROLL);
        ev->scroll.window = GDK_WINDOW(g_object_ref(gw));
        ev->scroll.direction = dir;
        ev->scroll.delta_y = dy;
        ev->scroll.state = mods;
        ev->scroll.x_root = ox + x;
        ev->scroll.y_root = oy + y;
        gdk_event_set_device(ev, gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())));
        const bool handled = gtk_widget_event(GTK_WIDGET(nb), ev);
        gdk_event_free(ev);
        pump();
        return handled;
    };
    auto current = [&] { return gtk_notebook_get_current_page(nb); };
    GtkWidget* label = gtk_notebook_get_tab_label(nb, gtk_notebook_get_nth_page(nb, 0));
    GtkAllocation l, c;
    gtk_widget_get_allocation(label, &l);
    const int x = l.x + l.width / 2, y = l.y + l.height / 2;
    gtk_notebook_set_current_page(nb, n - 1);
    pump();
    // In the document: left to the editor.
    gtk_widget_get_allocation(gtk_notebook_get_nth_page(nb, current()), &c);
    CHECK(!wheel(c.x + c.width / 2, c.y + c.height / 2, GDK_SCROLL_UP));
    CHECK(current() == n - 1);
    // Plain wheel: previous/next, stopping at the ends.
    CHECK(wheel(x, y, GDK_SCROLL_DOWN) && current() == n - 1);
    CHECK(wheel(x, y, GDK_SCROLL_UP) && current() == n - 2);
    CHECK(wheel(x, y, GDK_SCROLL_UP) && current() == n - 3);
    CHECK(wheel(x, y, GDK_SCROLL_DOWN) && current() == n - 2);
    CHECK(w->activeView()->widget() == gtk_notebook_get_nth_page(nb, n - 2));
    // Smooth scrolling (touchpads): a whole step switches.
    CHECK(wheel(x, y, GDK_SCROLL_SMOOTH, GdkModifierType(0), 0.5) && current() == n - 2);
    CHECK(wheel(x, y, GDK_SCROLL_SMOOTH, GdkModifierType(0), 0.5) && current() == n - 1);
    // Ctrl: wraps around.
    CHECK(wheel(x, y, GDK_SCROLL_DOWN, GDK_CONTROL_MASK) && current() == 0);
    CHECK(wheel(x, y, GDK_SCROLL_UP, GDK_CONTROL_MASK) && current() == n - 1);
    CHECK(wheel(x, y, GDK_SCROLL_UP) && current() == n - 2);
    // Ctrl+Shift: first/last.
    CHECK(wheel(x, y, GDK_SCROLL_UP, GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK)) && current() == 0);
    CHECK(wheel(x, y, GDK_SCROLL_DOWN, GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK)) && current() == n - 1);
    // Shift: moves the active tab, wrapping around.
    GtkWidget* moved = gtk_notebook_get_nth_page(nb, current());
    CHECK(wheel(x, y, GDK_SCROLL_DOWN, GDK_SHIFT_MASK) && current() == 0);
    CHECK(w->activeView()->widget() == moved && gtk_notebook_get_nth_page(nb, 0) == moved);
    CHECK(wheel(x, y, GDK_SCROLL_DOWN, GDK_SHIFT_MASK) && current() == 1);
    CHECK(gtk_notebook_get_nth_page(nb, 1) == moved);
    // With tabs locked Shift only switches, as the plain wheel.
    st.tabLock = true;
    CHECK(wheel(x, y, GDK_SCROLL_DOWN, GDK_SHIFT_MASK) && current() == 2);
    CHECK(gtk_notebook_get_nth_page(nb, 1) == moved);
    st.tabLock = false;
}

void testPanels(GtkApplication* app, core::LanguageCatalog& langs)
{
    Prompter p;
    auto w = makeWindow(app, langs, p);
    ui::DockManager& dock = w->dock();

    // Function List follows the active document.
    const fs::path cpp = g_dir / "panels" / "shapes.cpp";
    writeText(cpp, "class Shape {\npublic:\n    int area() { return 0; }\n    void draw() {}\n};\n\nint main()\n{\n    return 0;\n}\n");
    CHECK(w->openFiles({cpp}) == 1);
    CHECK(!w->commandChecked(IDM_VIEW_FUNC_LIST));
    CHECK(w->runCommand(IDM_VIEW_FUNC_LIST));
    CHECK(dock.isShown("functionList") && w->commandChecked(IDM_VIEW_FUNC_LIST));
    CHECK(dock.areaOf("functionList") == ui::DockManager::Area::Right);
    CHECK(w->functionList().dump() == "shapes.cpp\n  Shape\n    area\n    draw\n  main\n");
    w->functionList().setSorted(true);
    CHECK(w->functionList().dump() == "shapes.cpp\n  Shape\n    area\n    draw\n  main\n");
    w->functionList().setFilter("dr");
    CHECK(w->functionList().dump() == "shapes.cpp\n  Shape\n    draw\n");
    w->functionList().setFilter("");
    ui::EditorView& v = *w->activeView();
    GtkTreePath* mainRow = gtk_tree_path_new_from_string("0:1");
    CHECK(w->functionList().openRow(mainRow));
    gtk_tree_path_free(mainRow);
    CHECK(v.lineFromPos(v.currentPos()) == 6);
    // Edits re-parse after a pause.
    v.execute(SCI_DOCUMENTEND);
    v.execute(SCI_ADDTEXT, 16, reinterpret_cast<sptr_t>("void extra() {}\n"));
    CHECK(waitFor([&] { return w->functionList().dump().find("extra") != std::string::npos; }));

    // Document Map: shares the document, marks the visible part, scrolls the editor.
    std::string big;
    for (int i = 0; i < 400; ++i)
        big += "line " + std::to_string(i) + "\n";
    v.setText(big);
    CHECK(w->runCommand(IDM_VIEW_DOC_MAP));
    pump();
    ui::DocumentMap& map = w->documentMap();
    CHECK(map.view().execute(SCI_GETDOCPOINTER) == v.execute(SCI_GETDOCPOINTER));
    CHECK(waitFor([&] { map.sync(); return map.zone().second > 0; }));
    const sptr_t before = v.execute(SCI_GETFIRSTVISIBLELINE);
    map.scrollTo(map.zone().first + map.zone().second + 200);
    CHECK(v.execute(SCI_GETFIRSTVISIBLELINE) > before);

    // Document List: tabs in order, the active one marked.
    w->newDocument();
    CHECK(w->runCommand(IDM_VIEW_DOCLIST));
    CHECK(w->documentList().dump() == " shapes.cpp\n>new 1\n");
    w->activeView()->execute(SCI_ADDTEXT, 1, reinterpret_cast<sptr_t>("x"));
    pump();
    w->runCommand(IDM_VIEW_TAB1);
    CHECK(w->documentList().dump() == ">shapes.cpp\n *new 1\n"); // setText() left shapes.cpp unmodified
    // Closing the document the map shows detaches it.
    CHECK(w->runCommand(IDM_FILE_CLOSE));
    CHECK(w->documentList().dump() == ">*new 1\n");
    CHECK(map.view().execute(SCI_GETDOCPOINTER) == w->activeView()->execute(SCI_GETDOCPOINTER));

    // Character Panel: the document's code page, HTML forms; double-click inserts.
    CHECK(w->runCommand(IDM_EDIT_CHAR_PANEL));
    ui::CharPanel& cp = w->charPanel();
    CHECK(cp.charset() == "WINDOWS-1252");
    CHECK(cp.textFor(233, 2) == "\xC3\xA9" && cp.textFor(233, 3) == "&eacute;" && cp.textFor(233, 4) == "&#233;" && cp.textFor(233, 5) == "&#xE9;");
    CHECK(cp.textFor(128, 2) == "\xE2\x82\xAC" && cp.textFor(128, 4) == "&#8364;" && cp.textFor(65, 3) == "A"); // no name: the character
    cp.setCharset("WINDOWS-1251");
    CHECK(cp.textFor(233, 2) == "\xD0\xB9"); // й

    // Clipboard History: what is copied, newest first, no duplicates.
    CHECK(w->runCommand(IDM_EDIT_CLIPBOARDHISTORY_PANEL));
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    for (const char* t : {"first", "second", "first"}) {
        gtk_clipboard_set_text(cb, t, -1);
        CHECK(waitFor([&] { return !w->clipboardHistory().entries().empty() && w->clipboardHistory().entries().front() == t; }));
    }
    const auto& hist = w->clipboardHistory().entries();
    CHECK(hist.size() >= 2 && hist[0] == "first" && hist[1] == "second" && std::count(hist.begin(), hist.end(), "first") == 1);

    // Folder as Workspace: lazy tree, locate, refresh on changes, session.
    const fs::path ws = g_dir / "faw";
    writeText(ws / "b.txt", "b");
    writeText(ws / "A.txt", "a");
    writeText(ws / "src" / "main.c", "int main;");
    writeText(ws / ".hidden" / "x", "");
    ui::FolderPanel& fp = w->folderPanel();
    fp.addRoot(ws);
    CHECK(w->runCommand(IDM_VIEW_FILEBROWSER));
    CHECK(fp.dump() == "faw/\n  src/\n  A.txt\n  b.txt\n");
    CHECK(fp.locate(ws / "src" / "main.c"));
    CHECK(fp.dump() == "faw/\n  src/\n    main.c\n  A.txt\n  b.txt\n" && fp.selectedPath() == ws / "src" / "main.c");
    writeText(ws / "src" / "util.c", "");
    CHECK(waitFor([&] { return fp.dump().find("util.c") != std::string::npos; }));
    core::Session s = w->currentSession();
    CHECK(s.includeFileBrowser && s.fileBrowserRoots.size() == 1 && s.fileBrowserRoots[0].folder == ws.string());
    CHECK(s.fileBrowserRoots[0].expanded == std::vector<std::string>{(ws / "src").string()});
    fp.clear();
    CHECK(fp.dump().empty());
    fp.restoreFrom(s);
    CHECK(fp.dump().find("    main.c\n") != std::string::npos);

    // Project Panel: build, save in upstream format, reopen.
    ui::ProjectPanel& pr = w->projectPanel(1);
    CHECK(w->runCommand(IDM_VIEW_PROJECT_PANEL_1));
    const std::string proj = pr.addProject("App");
    const std::string folder = pr.addFolder(proj, "Sources");
    pr.addFiles(folder, {ws / "src" / "main.c"});
    pr.addDirectory(proj, ws);
    CHECK(pr.isDirty());
    CHECK(pr.dump() == "*Workspace\n  App\n    Sources\n      main.c\n    src\n      main.c\n      util.c\n    A.txt\n    b.txt\n");
    pr.move(folder, false);
    CHECK(pr.dump().find("  App\n    src\n") != std::string::npos);
    const fs::path wsFile = g_dir / "panels" / "my.workspace";
    CHECK(pr.saveWorkspace(wsFile) && !pr.isDirty());
    CHECK(readText(wsFile).find("<File name=\"../faw/src/main.c\"") == std::string::npos); // absolute when outside, as upstream
    pr.newWorkspace();
    CHECK(pr.openWorkspace(wsFile) && pr.dump().rfind("my.workspace\n  App\n", 0) == 0);
    CHECK(core::workspaceFiles(pr.workspace()).size() == 5);
    pr.rename("0:0", "Renamed");
    p.answers = {Answer::Cancel};
    CHECK(!w->requestQuit()); // a changed workspace: Cancel keeps the window
    pr.saveWorkspace();

    // Find in Projects: the workspace's files.
    core::FindOptions o;
    o.what = "main";
    CHECK(w->findDialog().doFindInFileList(o, core::workspaceFiles(pr.workspace())) == 2);

    // The dock: move, hide; the layout is saved and comes back.
    dock.moveTo("functionList", ui::DockManager::Area::Left);
    CHECK(dock.areaOf("functionList") == ui::DockManager::Area::Left && dock.isShown("functionList"));
    dock.moveTo("charPanel", ui::DockManager::Area::Float);
    pump();
    CHECK(dock.areaOf("charPanel") == ui::DockManager::Area::Float && dock.isShown("charPanel"));
    CHECK(w->runCommand(IDM_VIEW_DOC_MAP)); // toggles off
    CHECK(!dock.isShown("documentMap") && !w->commandChecked(IDM_VIEW_DOC_MAP));
    // These windows run without a session (-nosession): only the panels
    // ticked in Preferences > Multi-Instance & Date come back, as upstream.
    platform::Settings& ps = w->settings();
    ps.noSessionFuncList = ps.noSessionCharPanel = ps.noSessionDocList = ps.noSessionProjects = ps.noSessionDocMap = true;
    w.reset();

    auto w2 = makeWindow(app, langs, p);
    CHECK(w2->dock().areaOf("functionList") == ui::DockManager::Area::Left && w2->dock().isShown("functionList"));
    CHECK(w2->dock().areaOf("charPanel") == ui::DockManager::Area::Float && w2->dock().isShown("charPanel"));
    CHECK(!w2->dock().isShown("documentMap") && w2->dock().isShown("documentList"));
    CHECK(w2->commandChecked(IDM_VIEW_FUNC_LIST) && w2->commandChecked(IDM_VIEW_PROJECT_PANEL_1));
    CHECK(w2->projectPanel(1).workspaceFile() == wsFile && w2->projectPanel(1).dump().find("Renamed") != std::string::npos);
    for (const char* id : {"functionList", "charPanel", "documentList", "clipboardHistory", "folderAsWorkspace", "project1"})
        w2->dock().hide(id);
    w2->dock().moveTo("functionList", ui::DockManager::Area::Right);
    w2->dock().moveTo("charPanel", ui::DockManager::Area::Right);
}

void testCloseWithPanels(GtkApplication* app, core::LanguageCatalog& langs)
{
    // Closing the window as the user does (delete-event, then GTK destroys
    // it, then the MainWindow goes) touches no destroyed panel widget, and
    // the layout saved on close keeps the panels' tab order.
    int criticals = 0;
    auto count = +[](const gchar*, GLogLevelFlags, const gchar* msg, gpointer n) {
        ++*static_cast<int*>(n);
        std::fprintf(stderr, "unexpected: %s\n", msg);
    };
    std::vector<std::pair<const char*, guint>> handlers;
    for (const char* domain : {"Gtk", "GLib-GObject", "GLib", "Gdk"})
        handlers.emplace_back(domain, g_log_set_handler(domain, GLogLevelFlags(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING), count, &criticals));
    Prompter p;
    auto w = makeWindow(app, langs, p);
    for (int id : {IDM_VIEW_FUNC_LIST, IDM_VIEW_DOC_MAP, IDM_VIEW_DOCLIST, IDM_EDIT_CHAR_PANEL})
        if (!w->commandChecked(id))
            CHECK(w->runCommand(id));
    w->dock().moveTo("charPanel", ui::DockManager::Area::Float);
    pump();
    gtk_window_close(w->window());
    pump();
    const std::map<std::string, std::string> saved = w->settings().docking;
    w.reset();
    pump();
    for (const auto& [domain, id] : handlers)
        g_log_remove_handler(domain, id);
    CHECK(criticals == 0);
    // "area,shown,page,...": shown, with a real page number.
    for (const char* id : {"functionList", "documentMap", "documentList"}) {
        int area = -1, shown = -1, page = -1;
        auto it = saved.find(id);
        CHECK(it != saved.end() && std::sscanf(it->second.c_str(), "%d,%d,%d", &area, &shown, &page) == 3 && shown == 1 && page >= 0);
    }
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-ui-XXXXXX";
    const char* root = g_mkdtemp(tmpl);
    if (!root)
        return 2;
    g_setenv("XDG_CONFIG_HOME", (fs::path(root) / "config").c_str(), TRUE);
    g_setenv("NPP_GTK_DATA_DIR", NPP_TEST_DATA_DIR, TRUE);
    g_dir = fs::path(root) / "files";
    fs::create_directories(g_dir);

    gtk_init(&argc, &argv);
    core::LanguageCatalog langs;
    std::string err;
    CHECK(langs.load(fs::path(NPP_TEST_DATA_DIR) / "langs.model.xml", &err));
    GtkApplication* app = gtk_application_new("org.notepad_plus_plus.GtkPortUiTests", G_APPLICATION_NON_UNIQUE);
    CHECK(g_application_register(G_APPLICATION(app), nullptr, nullptr));

    std::printf("edit commands...\n");
    testEditCommands(app, langs);
    std::printf("find dialog and results...\n");
    testFindDialog(app, langs);
    std::printf("macros and run...\n");
    testMacrosAndRun(app, langs);
    std::printf("user defined languages...\n");
    testUserDefinedLanguages(app, langs);
    std::printf("device tree...\n");
    testDeviceTree(app, langs);
    std::printf("tab strip double click...\n");
    testTabStripDoubleClick(app, langs);
    testTabStripScroll(app, langs);
    std::printf("preferences...\n");
    testPreferences(app, langs);
    std::printf("panels...\n");
    testPanels(app, langs);
    std::printf("localization and shortcuts...\n");
    testLocalizationAndShortcuts(app, langs);
    std::printf("close with panels...\n");
    testCloseWithPanels(app, langs);

    g_object_unref(app);
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
