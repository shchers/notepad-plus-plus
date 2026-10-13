// Preferences: the editing aids (core/EditorFeatures) on a live Scintilla,
// settings.ini round trips, and the options' effect on a live MainWindow.

#include "core/EditorFeatures.h"
#include "core/LanguageCatalog.h"
#include "core/Translator.h"
#include "platform/FileIO.h"
#include "platform/Paths.h"
#include "platform/Settings.h"
#include "ui/MainWindow.h"

#include "menuCmdID.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace npp;

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
using Answer = ui::MainWindow::Answer;

void writeText(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

void pump()
{
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

bool hasIndicator(const core::Sci& s, int ind, sptr_t pos)
{
    return (s(SCI_INDICATORALLONFOR, static_cast<uptr_t>(pos)) & (1 << ind)) != 0;
}

std::vector<std::string> urls(const std::string& text, const std::string& schemes = {})
{
    std::vector<std::string> out;
    for (const auto& [start, len] : core::findUrls(text, schemes))
        out.push_back(text.substr(start, len));
    return out;
}

// Types `text` as the user would: each character inserted, then SCN_CHARADDED's work.
void typeText(ui::EditorView& v, const std::string& text)
{
    for (char c : text) {
        const char s[2] = {c, '\0'};
        v.execute(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(s)); // Scintilla sends SCN_CHARADDED only for keyboard input
        SCNotification n{};
        n.nmhdr.code = SCN_CHARADDED;
        n.ch = static_cast<unsigned char>(c);
        g_signal_emit_by_name(v.widget(), SCINTILLA_NOTIFY, 0, &n);
    }
}

void testUrls()
{
    CHECK((urls("see https://example.com/a?b=1&c=2#x, then") == std::vector<std::string>{"https://example.com/a?b=1&c=2#x"}));
    CHECK((urls("(http://a.org/x_(y)) and 'ftp://h/p'") == std::vector<std::string>{"http://a.org/x_(y)", "ftp://h/p"}));
    CHECK((urls("mailto:me@x.org; file:///tmp/a b") == std::vector<std::string>{"mailto:me@x.org", "file:///tmp/a"}));
    CHECK((urls("xhttp://no myssh://no") .empty()));
    CHECK((urls("git://host/repo.git") == std::vector<std::string>{}));
    CHECK((urls("git://host/repo.git", "git://") == std::vector<std::string>{"git://host/repo.git"}));
    CHECK((urls("é https://ü.example/ä") == std::vector<std::string>{"https://ü.example/ä"}));
}

void testEditorFeatures(core::LanguageCatalog& langs)
{
    core::StyleCatalog styles;
    ui::EditorView v;
    const core::Sci s = v.sci();

    // Smart highlighting: the selected word everywhere on screen.
    v.setText("alpha beta alpha alphabet Alpha\n");
    v.execute(SCI_SETSEL, 0, 5);
    core::smartHighlight(s, nullptr, true, false);
    CHECK(hasIndicator(s, core::kSmartIndicator, 11) && hasIndicator(s, core::kSmartIndicator, 27));
    CHECK(!hasIndicator(s, core::kSmartIndicator, 17)); // "alphabet": not a whole word
    core::smartHighlight(s, nullptr, true, true);
    CHECK(!hasIndicator(s, core::kSmartIndicator, 27)); // "Alpha": case differs
    v.execute(SCI_SETSEL, 0, 3);                          // part of a word: nothing in whole-word mode
    core::smartHighlight(s, nullptr, true, false);
    CHECK(!hasIndicator(s, core::kSmartIndicator, 11));
    core::smartHighlight(s, nullptr, false, false);
    CHECK(hasIndicator(s, core::kSmartIndicator, 17));

    // Matching tags.
    v.setText("<a x=\"1\"><b/><a>t</a></a>");
    v.execute(SCI_GOTOPOS, 2);
    core::highlightMatchingTags(s, true, true, false);
    CHECK(hasIndicator(s, core::kTagMatchIndicator, 1) && hasIndicator(s, core::kTagMatchIndicator, 23));
    CHECK(!hasIndicator(s, core::kTagMatchIndicator, 15));
    CHECK(hasIndicator(s, core::kTagAttrIndicator, 3));
    v.execute(SCI_GOTOPOS, 20); // inside "</a>" of the inner pair
    core::highlightMatchingTags(s, true, true, false);
    CHECK(hasIndicator(s, core::kTagMatchIndicator, 14) && hasIndicator(s, core::kTagMatchIndicator, 19));
    CHECK(!hasIndicator(s, core::kTagMatchIndicator, 1));
    v.setText("<p><!-- note --></p>");
    v.execute(SCI_GOTOPOS, 9);
    core::highlightMatchingTags(s, true, true, true);
    CHECK(hasIndicator(s, core::kTagMatchIndicator, 3) && hasIndicator(s, core::kTagMatchIndicator, 13));

    // Clickable links in the visible lines.
    v.setText("go to https://example.org now\n");
    core::markLinks(s, true, "");
    CHECK(core::linkAt(s, 10) == "https://example.org");
    CHECK(core::linkAt(s, 2).empty());
    core::markLinks(s, false, "");
    CHECK(core::linkAt(s, 10).empty());

    // Ctrl + double click: between delimiters.
    v.setText("f(a, g(b), c)\nx \"q\" y");
    CHECK(core::selectBetweenDelimiters(s, 7, '(', ')', false));
    CHECK(v.selectedText() == "b");
    CHECK(core::selectBetweenDelimiters(s, 3, '(', ')', false));
    CHECK(v.selectedText() == "a, g(b), c");
    CHECK(core::selectBetweenDelimiters(s, 17, '"', '"', false));
    CHECK(v.selectedText() == "q");
    CHECK(!core::selectBetweenDelimiters(s, 15, '[', ']', true));

    // Auto-indent, advanced: braces (C-like) and colons (Python).
    v.applyLanguage(*langs.byName("cpp"), langs, styles);
    ui::EditorOptions opts;
    opts.autoIndentMode = 2;
    opts.autoComplete.enable = false;
    opts.autoComplete.paramHint = false;
    v.setOptions(&opts);
    v.setText("");
    v.execute(SCI_SETEOLMODE, SC_EOL_LF);
    v.execute(SCI_SETTABWIDTH, 4);
    v.execute(SCI_SETUSETABS, 0);
    typeText(v, "if (x) {\nfoo();\n}");
    CHECK(v.text() == "if (x) {\n    foo();\n}");
    typeText(v, "\nwhile (y)\nbar();\nbaz();");
    CHECK(v.text() == "if (x) {\n    foo();\n}\nwhile (y)\n    bar();\nbaz();");
    v.applyLanguage(*langs.byName("python"), langs, styles);
    v.setText("");
    typeText(v, "def f():\nreturn 1");
    v.execute(SCI_COLOURISE, 0, -1);
    CHECK(v.text() == "def f():\n    return 1");
    opts.autoIndentMode = 0;
    v.setText("");
    typeText(v, "    a\nb");
    CHECK(v.text() == "    a\nb");

    // Auto-insert: pairs, stepping over them, user pairs, close tags.
    v.applyLanguage(*langs.byName("cpp"), langs, styles);
    opts.pairs.parentheses = opts.pairs.doubleQuotes = true;
    opts.pairs.user = {{'<', '>'}};
    v.setText("");
    typeText(v, "f(");
    CHECK(v.text() == "f()" && v.currentPos() == 2);
    typeText(v, "x)");
    CHECK(v.text() == "f(x)" && v.currentPos() == 4);
    typeText(v, " \"s\"");
    CHECK(v.text() == "f(x) \"s\"");
    typeText(v, " <");
    CHECK(v.text() == "f(x) \"s\" <>");
    v.applyLanguage(*langs.byName("html"), langs, styles);
    opts.pairs = {};
    opts.pairs.htmlXmlTag = true;
    v.setText("");
    typeText(v, "<div class=\"a\">");
    CHECK(v.text() == "<div class=\"a\"></div>");
    v.setText("");
    typeText(v, "<br>");
    CHECK(v.text() == "<br>"); // a void element
    v.setText("");
    v.applyLanguage(*langs.byName("xml"), langs, styles);
    typeText(v, "<a/>");
    CHECK(v.text() == "<a/>");

    // Auto-completion from upstream's API file, and the call tip.
    core::ApiCatalog apis(fs::path(NPP_TEST_DATA_DIR) / "autoCompletion");
    const core::ApiFile* cpp = apis.forLanguage("cpp");
    CHECK(cpp && !cpp->ignoreCase && cpp->find("strlen") && cpp->find("strlen")->func);
    CHECK(apis.forLanguage("javascript.js") != nullptr); // upstream's "javascript.xml"
    CHECK(apis.forLanguage("nosuchlanguage") == nullptr);
    v.applyLanguage(*langs.byName("cpp"), langs, styles);
    v.setApi(cpp);
    opts = {};
    opts.autoComplete.enable = true;
    opts.autoComplete.mode = 2;
    opts.autoComplete.fromChar = 3;
    v.setText("strlength stream\n");
    v.execute(SCI_DOCUMENTEND);
    typeText(v, "st");
    CHECK(!v.execute(SCI_AUTOCACTIVE)); // under "From 3rd character"
    typeText(v, "r");
    CHECK(v.execute(SCI_AUTOCACTIVE));
    v.execute(SCI_AUTOCCANCEL);
    typeText(v, "len(");
    CHECK(v.execute(SCI_CALLTIPACTIVE));
    v.execute(SCI_CALLTIPCANCEL);
    v.completer().closeCallTip(s);
    // Word completion on demand inserts the only candidate.
    v.setText("unique_identifier\nuni");
    v.execute(SCI_DOCUMENTEND);
    CHECK(v.completer().showWordList(s, nullptr, true, true));
    CHECK(v.text() == "unique_identifier\nunique_identifier");
    v.setOptions(nullptr);
}

void testSettingsAndTranslations()
{
    platform::Settings st;
    st.tabVertical = true;
    st.toolbarIconSet = 3;
    st.langHidden = {"cobol", "fortran"};
    st.langIndent["python"] = "4,1";
    st.pairOpen[1] = "<";
    st.dateFormat = "dd/MM/yyyy";
    st.currentLineMode = 2;
    st.autoIndentMode = 1;
    const fs::path f = g_dir / "settings.ini";
    CHECK(st.save(f));
    platform::Settings back;
    CHECK(back.load(f));
    CHECK(back.tabVertical && back.toolbarIconSet == 3 && back.langHidden == st.langHidden && back.langIndent == st.langIndent);
    CHECK(back.pairOpen[1] == "<" && back.dateFormat == "dd/MM/yyyy" && back.currentLineMode == 2 && back.currentLineHighlight);
    CHECK(back.autoIndentMode == 1 && back.autoIndent);
    // An older settings.ini: the boolean settings still decide.
    writeText(f, "[View]\ncurrentLineHighlight=false\nedgeColumn=100\n[Editing]\nautoIndent=false\n");
    platform::Settings old;
    CHECK(old.load(f));
    CHECK(old.currentLineMode == 0 && old.autoIndentMode == 0 && old.edgeColumns == "100");

    core::Translator tr;
    CHECK(tr.load(fs::path(NPP_TEST_DATA_DIR) / "nativeLang", ""));
    const auto items = tr.dialogComboItems("Preference/MISC", 6347);
    CHECK(items.size() == 3 && items[2] == "Disable");
    CHECK(tr.dialogComboItems("Preference/Print", 6724).size() == 7);

    // Edit > Insert > Date Time (customized): Windows date pictures.
    const std::string d = ui::MainWindow::formatCustomDate("yyyy-MM-dd'T'HH:mm 'x''y'");
    CHECK(d.size() == std::string("2026-10-13T12:34 x'y").size() && d[4] == '-' && d[10] == 'T' && d.substr(16) == " x'y");
}

std::unique_ptr<ui::MainWindow> makeWindow(GtkApplication* app, core::LanguageCatalog& langs, std::deque<Answer>* answers = nullptr)
{
    auto w = std::make_unique<ui::MainWindow>(app, langs, false);
    w->setPromptHandler([answers](const std::string&, const std::string&, bool) {
        if (!answers || answers->empty())
            return Answer::No;
        const Answer a = answers->front();
        answers->pop_front();
        return a;
    });
    w->present();
    pump();
    return w;
}

void testWindow(GtkApplication* app, core::LanguageCatalog& langs)
{
    std::deque<Answer> answers;
    auto w = makeWindow(app, langs, &answers);
    platform::Settings& st = w->settings();
    w->newDocument();
    ui::EditorView* v = w->activeView();

    // View options.
    st.currentLineMode = 2;
    st.currentLineFrame = 3;
    st.edgeMode = 1;
    st.edgeColumns = "80 100 120";
    st.paddingLeft = 7;
    st.lineNumberDynamic = false;
    st.changeHistoryMargin = true;
    st.backspaceUnindent = true;
    st.extraWordCharsOn = true;
    st.extraWordChars = "-";
    st.showNpc = true;
    w->applySettings();
    CHECK(v->execute(SCI_GETCARETLINEFRAME) == 3 && v->execute(SCI_GETCARETLINEVISIBLE));
    CHECK(v->execute(SCI_GETEDGEMODE) == EDGE_MULTILINE);
    CHECK(v->execute(SCI_GETMARGINLEFT) == 7 && v->execute(SCI_GETBACKSPACEUNINDENTS));
    CHECK(v->execute(SCI_GETMARGINWIDTHN, 2) > 0); // change history
    const sptr_t constantWidth = v->execute(SCI_GETMARGINWIDTHN, 0);
    st.lineNumberDynamic = true;
    w->applySettings();
    CHECK(v->execute(SCI_GETMARGINWIDTHN, 0) < constantWidth); // 3 digits instead of 4
    v->setText("a-b c\xC2\xA0" "d");
    CHECK(v->execute(SCI_WORDENDPOSITION, 0, true) == 3); // "-" is a word character now
    char rep[16] = {};
    v->execute(SCI_GETREPRESENTATION, reinterpret_cast<uptr_t>("\xC2\xA0"), reinterpret_cast<sptr_t>(rep));
    CHECK(std::string(rep) == "NBSP");
    st.npcDisplay = 1;
    w->applySettings();
    v->execute(SCI_GETREPRESENTATION, reinterpret_cast<uptr_t>("\xC2\xA0"), reinterpret_cast<sptr_t>(rep));
    CHECK(std::string(rep) == "U+00A0");
    CHECK(w->runCommand(IDM_VIEW_NPC) && !st.showNpc);

    // Tab bar, toolbar, menu, title.
    st.tabVertical = true;
    st.tabLock = true;
    st.hideMenu = true;
    st.titleFileNameOnly = true;
    st.toolbarIconSet = 1;
    w->applySettings();
    GtkWidget* nb = gtk_widget_get_parent(v->widget());
    CHECK(gtk_notebook_get_tab_pos(GTK_NOTEBOOK(nb)) == GTK_POS_LEFT);
    CHECK(!gtk_notebook_get_tab_reorderable(GTK_NOTEBOOK(nb), v->widget()));
    st.tabHide = true;
    w->applySettings();
    CHECK(!gtk_notebook_get_show_tabs(GTK_NOTEBOOK(nb)));
    st.tabHide = st.tabVertical = st.tabLock = st.hideMenu = false;
    w->applySettings();

    // Copy / cut the line without a selection.
    v->setText("one\ntwo\n");
    v->execute(SCI_GOTOPOS, 1);
    CHECK(w->runCommand(IDM_EDIT_CUT));
    CHECK(v->text() == "two\n");
    st.copyLineNoSelection = false;
    CHECK(w->runCommand(IDM_EDIT_CUT));
    CHECK(v->text() == "two\n");
    st.copyLineNoSelection = true;

    // Date time order, customized format.
    v->setText("");
    st.dateFormat = "'D:'yyyy";
    CHECK(w->runCommand(IDM_EDIT_INSERT_DATETIME_CUSTOMIZED));
    CHECK(v->text().rfind("D:20", 0) == 0 && v->text().size() == 6);

    // Fold toggling.
    v->setText("int f() {\n  a;\n}\n");
    v->applyLanguage(*langs.byName("cpp"), langs, core::StyleCatalog{});
    v->execute(SCI_COLOURISE, 0, -1);
    v->execute(SCI_GOTOPOS, 12);
    st.foldToggle = true;
    CHECK(w->runCommand(IDM_VIEW_FOLD_CURRENT));
    CHECK(!v->execute(SCI_GETFOLDEXPANDED, 0));
    CHECK(w->runCommand(IDM_VIEW_FOLD_CURRENT)); // toggles back
    CHECK(v->execute(SCI_GETFOLDEXPANDED, 0));

    // First line as the name of an untitled document.
    st.firstLineAsTabName = true;
    w->newDocument();
    ui::EditorView* u = w->activeView();
    u->execute(SCI_ADDTEXT, 12, reinterpret_cast<sptr_t>("My notes: x\n"));
    pump();
    CHECK(w->activeBuffer()->name() == "My notes x");
    st.firstLineAsTabName = false;

    // Large file restriction: no lexer, no wrap.
    st.largeFileRestrict = true;
    st.largeFileMB = 1;
    st.wordWrap = true;
    const fs::path big = g_dir / "big.cpp";
    writeText(big, "int x;\n" + std::string(1100 * 1024, 'a'));
    CHECK(w->openFiles({big}) == 1);
    CHECK(w->activeBuffer()->largeFile && w->activeBuffer()->language->isPlainText());
    CHECK(w->activeView()->execute(SCI_GETWRAPMODE) == SC_WRAP_NONE);
    const fs::path small = g_dir / "small.cpp";
    writeText(small, "int y;\n");
    CHECK(w->openFiles({small}) == 1);
    CHECK(!w->activeBuffer()->largeFile && w->activeBuffer()->language->name == "cpp");
    CHECK(w->activeView()->execute(SCI_GETWRAPMODE) == SC_WRAP_WORD);
    st.wordWrap = false;
    w->applySettings();

    // Ctrl+Tab in most-recently-used order.
    const core::Buffer* smallBuf = w->activeBuffer();
    CHECK(w->runCommand(IDM_VIEW_TAB_NEXT));
    pump();
    CHECK(w->activeBuffer()->path == big); // the previous one, not the next tab
    CHECK(w->activeBuffer() != smallBuf);

    // Save All asks first when configured (untitled documents closed first:
    // they would need a file dialog).
    w->runCommand(IDM_FILE_CLOSEALL);
    CHECK(w->openFiles({small}) == 1);
    st.saveAllConfirm = true;
    w->activeView()->execute(SCI_ADDTEXT, 1, reinterpret_cast<sptr_t>("z"));
    answers = {Answer::No};
    CHECK(w->runCommand(IDM_FILE_SAVEALL));
    CHECK(answers.empty() && w->isModified(*w->activeBuffer()));
    answers = {Answer::Yes};
    CHECK(w->runCommand(IDM_FILE_SAVEALL));
    CHECK(!w->isModified(*w->activeBuffer()));
    st.saveAllConfirm = false;

    // A folder: all its files, or Folder as Workspace.
    writeText(g_dir / "dir" / "a.txt", "a");
    writeText(g_dir / "dir" / "sub" / "b.txt", "b");
    writeText(g_dir / "dir" / ".hidden" / "c.txt", "c");
    const std::size_t before = w->documentCount();
    st.dropFolderOpensFiles = true;
    w->openFiles({g_dir / "dir"});
    CHECK(w->documentCount() == before + 2);
    st.dropFolderOpensFiles = false;
    w->openFiles({g_dir / "dir"});
    CHECK(w->documentCount() == before + 2 && w->dock().isShown("folderAsWorkspace"));

    // Session files by extension.
    st.sessionExt = "npps";
    const fs::path sess = g_dir / "mine.npps";
    CHECK(w->saveSession(sess));
    w->runCommand(IDM_FILE_CLOSEALL);
    w->openFiles({sess});
    CHECK(w->documentCount() >= 3);
    st.sessionExt.clear();

    // Recent files display.
    st.recentDisplay = 0;
    st.recentSubmenu = true;
    w->applySettings();

    // Language menu: compact or flat, and hidden languages.
    st.langMenuCompact = false;
    st.langHidden = {"cobol"};
    w->applySettings();
    st.langMenuCompact = true;
    st.langHidden.clear();
    w->applySettings();

    // Dark mode switches to the dark theme (at start, as the Dark Mode page does).
    st.darkMode = 1;
    st.darkTheme = "DarkModeDefault.xml";
    w.reset();
    auto w2 = makeWindow(app, langs);
    CHECK(w2->settings().theme == "DarkModeDefault.xml");
    w2->settings().darkMode = 0;
    w2.reset();
}

void testPreferencesDialog(GtkApplication* app, core::LanguageCatalog& langs)
{
    // The dialog builds every page; closed from a timer.
    auto w = makeWindow(app, langs);
    g_timeout_add(300, +[](gpointer) -> gboolean {
        GList* tl = gtk_window_list_toplevels();
        for (GList* l = tl; l; l = l->next)
            if (GTK_IS_DIALOG(l->data) && gtk_widget_get_visible(GTK_WIDGET(l->data)))
                gtk_dialog_response(GTK_DIALOG(l->data), GTK_RESPONSE_CLOSE);
        g_list_free(tl);
        return G_SOURCE_REMOVE;
    }, nullptr);
    CHECK(w->runCommand(IDM_SETTING_PREFERENCE));
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-prefs-XXXXXX";
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

    std::printf("links...\n");
    testUrls();
    std::printf("editing aids...\n");
    testEditorFeatures(langs);
    std::printf("settings and translations...\n");
    testSettingsAndTranslations();

    GtkApplication* app = gtk_application_new("org.notepad_plus_plus.GtkPortPrefsTests", G_APPLICATION_NON_UNIQUE);
    CHECK(g_application_register(G_APPLICATION(app), nullptr, nullptr));
    std::printf("options in the window...\n");
    testWindow(app, langs);
    std::printf("preferences dialog...\n");
    testPreferencesDialog(app, langs);
    g_object_unref(app);

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
