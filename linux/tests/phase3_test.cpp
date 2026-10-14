// Phase 3 core tests: search engine (modes, scopes, count, mark, find all,
// empty matches), Find in Files (filters, folders, replace with encodings),
// shortcuts.xml, macros, command tables, nativeLang and Edit menu operations.

#include "core/EditOps.h"
#include "core/FileSearch.h"
#include "core/KeyCombo.h"
#include "core/Macro.h"
#include "core/SearchEngine.h"
#include "core/Shortcuts.h"
#include "core/StyleCatalog.h"
#include "core/Translator.h"
#include "core/UserLanguage.h"
#include "platform/FileIO.h"
#include "ui/EditorView.h"

#include "FindReplaceDlg_rc.h"
#include "menuCmdID.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

void testExtended()
{
    using core::convertExtended;
    CHECK(convertExtended("a\\tb\\nc\\r") == "a\tb\nc\r");
    CHECK(convertExtended("\\\\") == "\\");
    CHECK(convertExtended("x\\0y") == std::string("x\0y", 3));
    CHECK(convertExtended("\\x41\\d066\\o103\\b01000100") == "ABCD");
    CHECK(convertExtended("\\u00e9") == "\xC3\xA9"); // é as UTF-8
    CHECK(convertExtended("\\q\\x4") == "\\q\\x4");   // unknown / too short: kept
}

void testSearchEngine()
{
    ui::EditorView v;
    core::Sci sci = v.sci();
    core::SearchEngine e(sci);
    v.setText("Foo foo food\nbar FOO\n");
    core::FindOptions o;
    o.what = "foo";
    CHECK(e.countAll(o) == 4);
    o.matchCase = true;
    CHECK(e.countAll(o) == 2);
    o.wholeWord = true;
    CHECK(e.countAll(o) == 1);
    o = {};
    o.what = "foo";
    o.wholeWord = true;
    CHECK(e.countAll(o) == 3);

    // Find next / previous with wrap.
    o = {};
    o.what = "bar";
    sci(SCI_SETSEL, 0, 0);
    CHECK(e.findNext(o) == core::FindStatus::Found);
    CHECK(v.selectedText() == "bar");
    CHECK(e.findNext(o) == core::FindStatus::Wrapped);
    o.wrap = false;
    CHECK(e.findNext(o) == core::FindStatus::NotFound);
    o.forward = false;
    sci(SCI_SETSEL, sci.length(), sci.length());
    CHECK(e.findNext(o) == core::FindStatus::Found && v.selectedText() == "bar");

    // Extended mode: newline in the pattern and the replacement.
    o = {};
    o.mode = core::SearchMode::Extended;
    o.what = "food\\nbar";
    o.with = "X\\tY";
    CHECK(e.replaceAll(o) == 1);
    CHECK(v.text() == "Foo foo X\tY FOO\n");
    sci(SCI_UNDO);
    CHECK(v.text() == "Foo foo food\nbar FOO\n");

    // Regex: group references, invalid patterns, empty matches.
    o = {};
    o.mode = core::SearchMode::Regex;
    o.what = "(f)(o+)";
    o.matchCase = true;
    o.with = "$2\\1";
    CHECK(e.replaceAll(o) == 2);
    CHECK(v.text() == "Foo oof oofd\nbar FOO\n");
    sci(SCI_UNDO);
    o.what = "(";
    CHECK(e.countAll(o) == -1);
    CHECK(e.findNext(o) == core::FindStatus::InvalidRegex);
    o.what = "$";
    o.with = ";";
    CHECK(e.replaceAll(o) == 3); // end of each of the 3 lines (last one empty)
    CHECK(v.text() == "Foo foo food;\nbar FOO;\n;");
    sci(SCI_UNDO);
    o.what = "^.*$";
    o.dotMatchesNewline = false;
    CHECK(e.countAll(o) == 3);
    o.what = "food.bar";
    CHECK(e.countAll(o) == 0);
    o.dotMatchesNewline = true;
    CHECK(e.countAll(o) == 1);

    // In selection: replace only inside it, keep the selection.
    o = {};
    o.what = "o";
    o.with = "0";
    o.inSelection = true;
    sci(SCI_SETSEL, 4, 12); // "foo food"
    CHECK(e.replaceAll(o) == 4);
    CHECK(v.text() == "Foo f00 f00d\nbar FOO\n");
    CHECK(sci(SCI_GETSELECTIONSTART) == 4 && sci(SCI_GETSELECTIONEND) == 12);
    sci(SCI_UNDO);

    // Replace: selection that is a match is replaced, then the next one found.
    o = {};
    o.what = "foo";
    o.with = "bar";
    o.matchCase = true;
    sci(SCI_SETSEL, 4, 7);
    CHECK(e.replace(o) == core::FindStatus::Found);
    CHECK(v.text().rfind("Foo bar food", 0) == 0);
    CHECK(sci(SCI_GETSELECTIONSTART) == 8);
    sci(SCI_UNDO);

    // Mark all with bookmarks; purge clears the previous marks.
    o = {};
    o.what = "foo";
    o.bookmarkLine = true;
    CHECK(e.markAll(o, core::kFoundIndicator, ui::EditorView::kBookmarkMarker) == 4);
    sci(SCI_SETINDICATORCURRENT, core::kFoundIndicator);
    CHECK(sci(SCI_INDICATORVALUEAT, core::kFoundIndicator, 0) != 0);
    CHECK(sci(SCI_INDICATORVALUEAT, core::kFoundIndicator, 3) == 0);
    CHECK((v.bookmarkLines() == std::vector<sptr_t>{0, 1}));
    o.what = "bar";
    o.purge = true;
    o.bookmarkLine = false;
    CHECK(e.markAll(o) == 1);
    CHECK(sci(SCI_INDICATORVALUEAT, core::kFoundIndicator, 0) == 0);
    e.clearMarks();
    CHECK(sci(SCI_INDICATORVALUEAT, core::kFoundIndicator, 13) == 0);

    // Find all: lines and match offsets.
    std::vector<core::Hit> hits;
    o = {};
    o.what = "FOO";
    o.matchCase = true;
    CHECK(e.findAll(o, hits) == 1);
    CHECK(hits.size() == 1 && hits[0].line == 1 && hits[0].lineText == "bar FOO");
    CHECK(hits[0].matchStartInLine == 4 && hits[0].matchEndInLine == 7);

    // Read-only documents are not replaced.
    sci(SCI_SETREADONLY, 1);
    o.with = "x";
    CHECK(e.replaceAll(o) == 0);
    sci(SCI_SETREADONLY, 0);
}

void testFileSearch()
{
    const fs::path root = g_dir / "fif";
    writeText(root / "a.cpp", "int foo;\nfoo();\n");
    writeText(root / "b.H", "// foo\n");
    writeText(root / "c.txt", "foo\n");
    writeText(root / "sub" / "d.cpp", "foo foo\n");
    writeText(root / "build" / "e.cpp", "foo\n");
    writeText(root / "sub" / "build" / "f.cpp", "foo\n");
    writeText(root / ".git" / "g.cpp", "foo\n");
    writeText(root / "bin.cpp", std::string("foo\0\0\x01", 6));
    writeText(root / "latin.txt", "caf\xE9 foo\n");
    writeText(root / "Makefile", "foo:\n");

    auto names = [&](const core::FileFilter& f, bool recursive, bool hidden) {
        std::vector<std::string> out;
        for (const auto& c : core::listFiles(root, f, recursive, hidden))
            out.push_back(fs::relative(c.path, root).string());
        return out;
    };
    CHECK((names(core::FileFilter("*.cpp *.h"), false, false) == std::vector<std::string>{"a.cpp", "b.H", "bin.cpp"}));
    CHECK(names(core::FileFilter("*.cpp"), true, false).size() == 5); // a, bin, build/e, sub/d, sub/build/f
    CHECK(names(core::FileFilter("*.cpp"), true, true).size() == 6);  // + .git/g
    CHECK(names(core::FileFilter("*.cpp !\\build"), true, false).size() == 4);  // top-level build only
    CHECK(names(core::FileFilter("*.cpp !+\\build"), true, false).size() == 3); // every level
    CHECK(names(core::FileFilter("*.cpp !bin*"), true, false).size() == 4);
    CHECK(names(core::FileFilter(""), false, false).size() == 6);     // "*.*": all, Makefile too
    CHECK(names(core::FileFilter("!*.txt"), false, false).size() == 4); // exclusions only: all but those

    ui::EditorView scratch;
    core::FileSearcher fs(scratch.sci());
    core::FindOptions o;
    o.what = "foo";
    core::FileHits h;
    CHECK(fs.findInFile(root / "a.cpp", o, h));
    CHECK(h.hits.size() == 2 && h.hits[1].line == 1);
    CHECK(!fs.findInFile(root / "bin.cpp", o, h)); // binary: skipped
    CHECK(fs.findInFile(root / "latin.txt", o, h) && h.hits.size() == 1 && h.hits[0].lineText == "caf\xC3\xA9 foo");

    // Replace in files keeps the file's encoding and line endings.
    writeText(root / "crlf.txt", "foo\r\nbar foo\r\n");
    o.with = "baz";
    CHECK(fs.replaceInFile(root / "latin.txt", o) == 1);
    CHECK(readText(root / "latin.txt") == "caf\xE9 baz\n");
    CHECK(fs.replaceInFile(root / "crlf.txt", o) == 2);
    CHECK(readText(root / "crlf.txt") == "baz\r\nbar baz\r\n");
    CHECK(fs.replaceInFile(root / "c.txt", core::FindOptions{.what = "nothing"}) == 0);
    o.mode = core::SearchMode::Regex;
    o.what = "(";
    CHECK(fs.replaceInFile(root / "c.txt", o) == -1);
}

void testShortcutsAndMacros()
{
    // Defaults come from upstream's winKeyDefs.
    core::CommandKeys keys;
    auto has = [&](int id, core::KeyCombo k) {
        for (const auto& c : keys.keysOf(id))
            if (c == k)
                return true;
        return false;
    };
    CHECK(has(IDM_FILE_NEW, {true, false, false, 'N'}));
    CHECK(has(IDM_EDIT_CUT, {true, false, false, 'X'}) && has(IDM_EDIT_CUT, {false, false, true, 0x2E}));
    CHECK(has(IDM_SEARCH_FINDNEXT, {false, false, false, 0x72}));
    CHECK(keys.changes().empty());
    keys.set(IDM_FILE_NEW, 0, {true, true, false, 'N'});
    CHECK(keys.changes().size() == 1);
    CHECK(keys.conflicts({true, false, false, 'O'}).size() == 1);
    CHECK(core::KeyCombo({true, false, true, 0x72}).toString() == "Ctrl+Shift+F3");
    CHECK(core::KeyCombo({false, true, false, 0x6B}).toString() == "Alt+Num +");

    // shortcuts.xml round trip, keeping sections the port does not use.
    const fs::path f = g_dir / "shortcuts.xml";
    writeText(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<NotepadPlus>\n"
                 "  <InternalCommands><Shortcut id=\"41001\" Ctrl=\"yes\" Alt=\"yes\" Shift=\"no\" Key=\"78\" /></InternalCommands>\n"
                 "  <Macros><Macro name=\"Trim Trailing Space and Save\" Ctrl=\"no\" Alt=\"yes\" Shift=\"yes\" Key=\"83\">\n"
                 "    <Action type=\"2\" message=\"0\" wParam=\"42024\" lParam=\"0\" sParam=\"\" />\n"
                 "    <Action type=\"1\" message=\"2170\" wParam=\"0\" lParam=\"0\" sParam=\"&#x000D;\" />\n"
                 "    <Action type=\"1\" message=\"2170\" wParam=\"0\" lParam=\"0\" sParam=\"&#x000A;\" />\n"
                 "    <Action type=\"2\" message=\"0\" wParam=\"41006\" lParam=\"0\" sParam=\"\" />\n"
                 "  </Macro></Macros>\n"
                 "  <UserDefinedCommands><Command name=\"Open in browser\" Ctrl=\"yes\" Alt=\"yes\" Shift=\"yes\" Key=\"88\">xdg-open &quot;$(FULL_CURRENT_PATH)&quot;</Command></UserDefinedCommands>\n"
                 "  <PluginCommands><PluginCommand moduleName=\"x.dll\" internalID=\"1\" Ctrl=\"no\" Alt=\"no\" Shift=\"no\" Key=\"0\" /></PluginCommands>\n"
                 "  <ScintillaKeys><ScintKey ScintID=\"2180\" menuCmdID=\"0\" Ctrl=\"no\" Alt=\"no\" Shift=\"no\" Key=\"0\" /></ScintillaKeys>\n"
                 "</NotepadPlus>\n");
    core::ShortcutsFile sf;
    CHECK(sf.load(f));
    CHECK(sf.commands.size() == 1 && sf.commands[0].id == IDM_FILE_NEW && sf.commands[0].key.alt);
    CHECK(sf.macros.size() == 1 && sf.macros[0].key.toString() == "Alt+Shift+S");
    // "\r" then "\n" typed separately become one SCI_NEWLINE (upstream getActions).
    CHECK(sf.macros[0].steps.size() == 3 && sf.macros[0].steps[1].message == SCI_NEWLINE);
    CHECK(sf.userCommands.size() == 1 && sf.userCommands[0].command == "xdg-open \"$(FULL_CURRENT_PATH)\"");
    CHECK(sf.pluginCommands.size() == 1 && sf.pluginCommands[0].module == "x.dll" && sf.pluginCommands[0].internalId == 1);
    CHECK(sf.pluginKey("X.DLL", 1) && !sf.pluginKey("x.dll", 2)); // module names compare case-insensitively, as upstream
    CHECK(sf.save(f));
    const std::string written = readText(f);
    CHECK(written.find("x.dll") != std::string::npos && written.find("ScintID=\"2180\"") != std::string::npos);
    // Newlines inside text steps must survive a save as character references.
    sf.macros[0].steps.push_back(core::MacroStep{core::MacroStep::UseSParam, SCI_REPLACESEL, 0, 0, "a\tb\r\nc"});
    CHECK(sf.save(f));
    core::ShortcutsFile again;
    CHECK(again.load(f));
    CHECK(again.macros[0].steps == sf.macros[0].steps && again.userCommands[0].command == sf.userCommands[0].command);
    CHECK(again.macros[0].steps.back().sParam == "a\tb\r\nc");

    // Macro steps from Scintilla notifications.
    const char* typed = "abc";
    auto step = core::MacroStep::fromScintilla(SCI_REPLACESEL, 0, reinterpret_cast<sptr_t>(typed));
    CHECK(step.type == core::MacroStep::UseSParam && step.sParam == "abc" && step.lParam == 0);
    CHECK(step.isPlayableScintilla());
    CHECK(core::MacroStep::fromScintilla(SCI_CHARRIGHT, 0, 0).isPlayableScintilla());
    CHECK(!core::MacroStep::fromScintilla(SCI_SETZOOM, 3, 0).isPlayableScintilla());
    CHECK(core::isRecordableCommand(IDM_EDIT_TRIMTRAILING));
    CHECK(core::isRecordableCommand(IDM_FILE_SAVE));
    CHECK(!core::isRecordableCommand(IDM_MACRO_STARTRECORDINGMACRO));
}

void testTranslator()
{
    const fs::path dir = fs::path(NPP_TEST_DATA_DIR) / "nativeLang";
    core::Translator t;
    CHECK(t.load(dir, ""));
    CHECK(t.command(IDM_FILE_NEW, "?") == "&New");
    CHECK(t.menu("file", "?") == "&File");
    CHECK(t.subMenu("edit-lineOperations", "?") == "&Line Operations");
    CHECK(t.dialog("Find", IDFINDWHAT, "?") != "?" || t.dialog("Find", 1620, "?") == "&Find what:");
    CHECK(t.dialogAttr("Find", "titleReplace", "?") == "Replace");
    CHECK(core::Translator::plain(t.dialog("Preference/Global", 6123, "?")) == "Localization"); // upstream now has "&Localization"
    CHECK(t.command(999999, "fallback") == "fallback");
    CHECK(core::Translator::toMnemonic("Save &As...\tCtrl+Alt+S") == "Save _As...");
    CHECK(core::Translator::toMnemonic("Copy && paste my_file") == "Copy & paste my__file");
    CHECK(core::Translator::plain("&File") == "File");
    CHECK(core::Translator::format("Count: $INT_REPLACE$ matches", "$INT_REPLACE$", "3") == "Count: 3 matches");

    core::Translator fr;
    CHECK(fr.load(dir, "french.xml"));
    CHECK(fr.languageName() == "French" || fr.languageName() == "Français");
    CHECK(fr.command(IDM_FILE_NEW, "?") != "&New");
    CHECK(fr.command(IDM_FILE_NEW, "?") != "?");
    CHECK(!fr.load(dir, "../english.xml")); // never outside the folder
    const auto langs = core::Translator::available(dir);
    CHECK(langs.size() > 80);
}

void testEditOps()
{
    using core::TextCase;
    CHECK(core::convertCase("hello wORLD", TextCase::Upper) == "HELLO WORLD");
    CHECK(core::convertCase("ÉCOLE Été", TextCase::Lower) == "école été");
    CHECK(core::convertCase("don't stop mcDONALD", TextCase::ProperForce) == "Don't Stop Mcdonald");
    CHECK(core::convertCase("don't stop mcDONALD", TextCase::ProperBlend) == "Don't Stop McDONALD");
    CHECK(core::convertCase("hello. i think SO! yes", TextCase::SentenceForce) == "Hello. I think so! Yes");
    CHECK(core::convertCase("hello. i think SO", TextCase::SentenceBlend) == "Hello. I think SO");
    CHECK(core::convertCase("aBc", TextCase::Invert) == "AbC");

    std::vector<std::string> l = {"b10", "a2", "b9", "B1"};
    CHECK(core::sortStrings(l, core::SortKind::Lexicographic, false) && (l == std::vector<std::string>{"B1", "a2", "b10", "b9"}));
    CHECK(core::sortStrings(l, core::SortKind::LexicographicIgnoreCase, false) && (l == std::vector<std::string>{"a2", "B1", "b10", "b9"}));
    CHECK(core::sortStrings(l, core::SortKind::Integer, false) && (l == std::vector<std::string>{"B1", "a2", "b9", "b10"}));
    l = {"10", "-3", "", "2"};
    CHECK(core::sortStrings(l, core::SortKind::Integer, true) && (l == std::vector<std::string>{"10", "2", "-3", ""}));
    l = {"1,5", "", "-2", "1,25"};
    CHECK(core::sortStrings(l, core::SortKind::DecimalComma, false) && (l == std::vector<std::string>{"", "-2", "1,25", "1,5"}));
    l = {"1.5", "-"}; // no number at all ("abc") counts as blank, as upstream
    std::size_t bad = 99;
    CHECK(!core::sortStrings(l, core::SortKind::DecimalDot, false, 0, 0, &bad) && bad == 1);
    l = {"xx3", "yy1", "zz2"}; // by columns [2, 3)
    CHECK(core::sortStrings(l, core::SortKind::Lexicographic, false, 2, 3) && (l == std::vector<std::string>{"yy1", "zz2", "xx3"}));
    l = {"ccc", "a", "bb"};
    CHECK(core::sortStrings(l, core::SortKind::Length, true) && (l == std::vector<std::string>{"ccc", "bb", "a"}));

    ui::EditorView v;
    core::Sci sci = v.sci();
    v.setText("c\nb\na\n");
    CHECK(core::sortLines(sci, core::SortKind::Lexicographic, false));
    CHECK(v.text() == "\na\nb\nc"); // whole document: the empty last line sorts too, as upstream
    v.setText("c\nb\na\nz\n");
    sci(SCI_SETSEL, 0, sci.lineStart(3)); // first three lines; ends at column 0 of line 4
    CHECK(core::sortLines(sci, core::SortKind::Lexicographic, false));
    CHECK(v.text() == "a\nb\nc\nz\n");
    v.setText("1\n-\n");
    std::string err;
    CHECK(!core::sortLines(sci, core::SortKind::DecimalDot, false, &err) && err == "2" && v.text() == "1\n-\n");

    v.setText("  a  \n\tb\t\n");
    core::trim(sci, core::Trim::Trailing);
    CHECK(v.text() == "  a\n\tb\n");
    core::trim(sci, core::Trim::Leading);
    CHECK(v.text() == "a\nb\n");
    v.setText(" a \n b \nc");
    core::trimAll(sci);
    CHECK(v.text() == "a b c");

    sci(SCI_SETTABWIDTH, 4);
    v.setText("\tx\ty");
    core::tabsToSpaces(sci);
    CHECK(v.text() == "    x   y");
    v.setText("        x   y z");
    core::spacesToTabs(sci, true);
    CHECK(v.text() == "\t\tx   y z");
    v.setText("        x   y z");
    core::spacesToTabs(sci, false);
    CHECK(v.text() == "\t\tx\ty z");
    v.setText("ab \tc"); // absorbed by the TAB
    core::spacesToTabs(sci, false);
    CHECK(v.text() == "ab\tc");
    v.setText("ab  \tc"); // reaches a tab stop first
    core::spacesToTabs(sci, false);
    CHECK(v.text() == "ab\t\tc");

    v.setText("a\n\n  \nb\n\n");
    core::removeEmptyLines(sci, false);
    CHECK(v.text() == "a\n  \nb");
    core::removeEmptyLines(sci, true);
    CHECK(v.text() == "a\nb");
    v.setText("a\nb\na\na\nb");
    core::removeDuplicateLines(sci, true);
    CHECK(v.text() == "a\nb\na\nb");
    core::removeDuplicateLines(sci, false);
    CHECK(v.text() == "a\nb");
    sci(SCI_UNDO);
    CHECK(v.text() == "a\nb\na\nb");

    v.setText("one\ntwo\nthree");
    sci(SCI_SETSEL, 0, sci.length());
    core::joinLines(sci);
    CHECK(v.text() == "one two three");
    v.setText("x");
    sci(SCI_GOTOPOS, 0);
    core::insertBlankLine(sci, true);
    CHECK(v.text() == "\nx");
    core::insertBlankLine(sci, false);
    CHECK(v.text() == "\n\nx");

    v.setText("  a\n\n  b\n");
    sci(SCI_SETSEL, 0, sci.length());
    CHECK(core::lineComment(sci, "//", core::LineComment::Toggle));
    CHECK(v.text() == "  // a\n\n  // b\n");
    CHECK(core::lineComment(sci, "//", core::LineComment::Toggle));
    CHECK(v.text() == "  a\n\n  b\n");
    CHECK(!core::lineComment(sci, "", core::LineComment::Add));
    v.setText("int x;");
    sci(SCI_SETSEL, 4, 5);
    CHECK(core::streamComment(sci, "/*", "*/", true));
    CHECK(v.text() == "int /* x */;");
    sci(SCI_GOTOPOS, 7);
    CHECK(core::streamComment(sci, "/*", "*/", false));
    CHECK(v.text() == "int x;");

    v.setText("l0\nl1\nl2\nl3");
    sci(SCI_MARKERADD, 1, 24);
    sci(SCI_MARKERADD, 3, 24);
    CHECK(core::markedLines(sci, 24) == (std::vector<sptr_t>{1, 3}));
    CHECK(core::copyMarkedLines(sci, 24) == "l1\nl3");
    core::replaceMarkedLines(sci, 24, "X");
    CHECK(v.text() == "l0\nX\nl2\nX");
    core::inverseMarks(sci, 24);
    CHECK(core::markedLines(sci, 24) == (std::vector<sptr_t>{0, 2}));
    core::deleteMarkedLines(sci, 24, false);
    CHECK(v.text() == "l0\nl2\n");
    core::deleteMarkedLines(sci, 24, true);
    CHECK(v.text() == "");
}

void testColumnEditor()
{
    using core::Leading;
    using core::NumberBase;
    CHECK(core::formatColumnNumber(7, NumberBase::Dec, 3, Leading::Zeros) == "007");
    CHECK(core::formatColumnNumber(7, NumberBase::Dec, 3, Leading::Spaces) == "  7");
    CHECK(core::formatColumnNumber(7, NumberBase::Dec, 3, Leading::None) == "7  ");
    CHECK(core::formatColumnNumber(255, NumberBase::Hex, 2, Leading::None) == "ff");
    CHECK(core::formatColumnNumber(255, NumberBase::Hex, 2, Leading::None, true) == "FF");
    CHECK(core::formatColumnNumber(5, NumberBase::Bin, 4, Leading::Zeros) == "0101");
    CHECK(core::formatColumnNumber(8, NumberBase::Oct, 1, Leading::None) == "10");

    ui::EditorView v;
    core::Sci sci = v.sci();
    // Caret mode: from the caret line down, at the caret column; short lines padded.
    v.setText("abcd\nab\nabcd");
    sci(SCI_GOTOPOS, 3);
    core::columnInsertText(sci, "|");
    CHECK(v.text() == "abc|d\nab |\nabc|d");
    // Numbers in a column selection, repeated and zero-padded.
    v.setText("a\nb\nc\nd\ne");
    sci(SCI_SETSELECTIONMODE, SC_SEL_RECTANGLE);
    sci(SCI_SETRECTANGULARSELECTIONANCHOR, 0);
    sci(SCI_SETRECTANGULARSELECTIONCARET, sci.lineStart(4));
    CHECK(core::columnInsertNumbers(sci, 8, 1, 2, NumberBase::Dec, Leading::Zeros));
    CHECK(v.text() == "08a\n08b\n09c\n09d\n10e");
    CHECK(sci(SCI_GETSELECTIONS) == 5); // the inserted numbers stay selected
    sci(SCI_UNDO);
    CHECK(v.text() == "a\nb\nc\nd\ne");
    CHECK(!core::columnInsertNumbers(sci, 1, 1, 0, NumberBase::Dec, Leading::None));
}

void testStyleCatalogSave()
{
    core::StyleCatalog c;
    std::string err;
    CHECK(c.load(fs::path(NPP_TEST_DATA_DIR) / "stylers.model.xml", &err));
    CHECK(!c.lexers().empty() && c.lexers().front().name == "actionscript");
    std::vector<core::Style>* cpp = c.mutableLexer("cpp");
    CHECK(cpp && !cpp->empty());
    if (!cpp)
        return;
    (*cpp)[0].fg = 0x00FF00u; // BGR: green
    (*cpp)[0].fontStyle = core::kBold | core::kItalic;
    std::size_t kw = 0;
    for (std::size_t i = 0; i < cpp->size(); ++i)
        if ((*cpp)[i].keywordClass >= 0)
            kw = i;
    (*cpp)[kw].userKeywords = "myType yourType";
    c.mutableGlobals()[0].bg = 0x112233u;
    c.setLexerExt("cpp", "cxxm");
    const fs::path out = g_dir / "styles" / "stylers.xml";
    CHECK(c.save(out, &err));
    core::StyleCatalog r;
    CHECK(r.load(out, &err));
    const auto* rc = r.forLanguage("cpp");
    CHECK(rc && (*rc)[0].fg == 0x00FF00u && (*rc)[0].fontStyle == (core::kBold | core::kItalic));
    CHECK(rc && (*rc)[kw].userKeywords == "myType yourType" && (*rc)[kw].keywordClass >= 0);
    CHECK(r.globals()[0].bg == 0x112233u);
    CHECK(core::StyleCatalog::toHex(0x112233u) == "332211");
    bool extOk = false;
    for (const auto& l : r.lexers())
        if (l.name == "cpp")
            extOk = l.ext == "cxxm";
    CHECK(extOk);
    // Untouched content survives (attributes the port does not model).
    CHECK(readText(out).find("<LexerType name=\"python\" desc=\"Python\"") != std::string::npos);
}

void testUserLanguages()
{
    const auto udls = core::loadAllUserLanguages(g_dir / "no-config", {fs::path(NPP_TEST_DATA_DIR) / "userDefineLangs"});
    CHECK(udls.size() == 4); // Markdown and Device Tree, light and dark
    const core::UserLanguage* md = nullptr;
    for (const auto& u : udls)
        if (u.name == "Markdown (preinstalled)")
            md = &u;
    CHECK(md != nullptr);
    if (!md)
        return;
    CHECK(md->matchesExtension(".md") && md->matchesExtension("MARKDOWN") && !md->matchesExtension(".txt"));
    CHECK(md->caseIgnored && md->forcePureLC == 2 && md->isPrefix[0] && !md->isPrefix[7]);
    CHECK(md->keywordLists[SCE_USER_KWLIST_COMMENTS].find("00#") == 0);
    CHECK(md->styles.size() == SCE_USER_STYLE_TOTAL_STYLES && md->styles[SCE_USER_STYLE_COMMENTLINE].fg.has_value());

    const core::UserLexerSetup plan = core::planUserLexer(*md, 7, 9);
    auto prop = [&](const std::string& k) {
        for (const auto& [key, v] : plan.properties)
            if (key == k)
                return v;
        return std::string("?");
    };
    CHECK(prop("userDefine.isCaseIgnored") == "1" && prop("userDefine.udlName") == "7" && prop("userDefine.currentBufferID") == "9");
    CHECK(prop("userDefine.comments").find("00#") == 0);
    CHECK(prop("userDefine.nesting.01") != "?");
    CHECK(plan.keywordSets.size() == SCE_USER_KWLIST_TOTAL - 13);

    // Quoted keywords keep inner spaces, encoded for LexUser; old-style files load too.
    writeText(g_dir / "udl" / "old.xml",
              "<NotepadPlus><UserLang name=\"Old\" ext=\"old\">"
              "<Settings><Global caseIgnored=\"no\" /><Prefix words1=\"yes\" /></Settings>"
              "<KeywordLists><Keywords name=\"Words1\">\"two words\" 'x y' plain</Keywords>"
              "<Keywords name=\"Delimiters\">\"00000</Keywords><Keywords name=\"Comment\">1// 2</Keywords></KeywordLists>"
              "<Styles><WordsStyle name=\"KEYWORD1\" fgColor=\"FF0000\" /></Styles></UserLang>"
              "<UserLang name=\"Broken\" /></NotepadPlus>");
    const auto old = core::loadUserLanguages(g_dir / "udl" / "old.xml");
    CHECK(old.size() == 1 && old[0].isPrefix[0]);
    if (old.size() == 1) {
        const core::UserLexerSetup p2 = core::planUserLexer(old[0], 1, 1);
        CHECK(std::find(p2.keywordSets.begin(), p2.keywordSets.end(), "two\vwords x\by plain") != p2.keywordSets.end());
        CHECK(old[0].keywordLists[SCE_USER_KWLIST_DELIMITERS].rfind("00\"", 0) == 0);
        CHECK(old[0].keywordLists[SCE_USER_KWLIST_COMMENTS] == "03// 04 01 02");
        CHECK(old[0].styles[SCE_USER_STYLE_KEYWORD1].fg == 0x0000FFu);
    }

    // Comments do not move the later fold levels a line (fixed in
    // lexilla/lexers/LexUser.cxx): a block comment closing at an LF line end
    // (counted twice), a line comment ending at ((EOL)) (not counted).
    writeText(g_dir / "udl" / "braces.xml",
              "<NotepadPlus><UserLang name=\"Braces\" ext=\"br\" udlVersion=\"2.1\"><Settings /><Styles /><KeywordLists>"
              "<Keywords name=\"Comments\">00// 01 02((EOL)) 03/* 04*/</Keywords>"
              "<Keywords name=\"Folders in code1, open\">{</Keywords>"
              "<Keywords name=\"Folders in code1, close\">}</Keywords>"
              "</KeywordLists></UserLang></NotepadPlus>");
    const auto braces = core::loadUserLanguages(g_dir / "udl" / "braces.xml");
    CHECK(braces.size() == 1);
    if (braces.size() == 1) {
        const core::StyleCatalog styles;
        for (const char* eol : {"\n", "\r\n"}) {
            ui::EditorView v;
            std::string text;
            for (const char* line : {"/* one */", "a {", "/*", " * two", " */", "// three", "b {", "}", "}"})
                text += std::string(line) + eol;
            v.setText(text);
            v.applyUserLanguage(braces[0], styles, 1);
            auto header = [&](int line) { return (v.execute(SCI_GETFOLDLEVEL, static_cast<uptr_t>(line)) & SC_FOLDLEVELHEADERFLAG) != 0; };
            CHECK(header(1) && !header(2) && !header(5) && header(6) && !header(7));
            CHECK((v.execute(SCI_GETFOLDLEVEL, 8) & SC_FOLDLEVELNUMBERMASK) == SC_FOLDLEVELBASE + 1);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    char tmpl[] = "/tmp/nppgtk-p3-XXXXXX";
    const char* root = g_mkdtemp(tmpl);
    if (!root)
        return 2;
    g_dir = root;
    gtk_init(&argc, &argv);

    std::printf("extended mode...\n");
    testExtended();
    std::printf("search engine...\n");
    testSearchEngine();
    std::printf("find in files...\n");
    testFileSearch();
    std::printf("shortcuts and macros...\n");
    testShortcutsAndMacros();
    std::printf("translations...\n");
    testTranslator();
    std::printf("edit operations...\n");
    testEditOps();
    std::printf("column editor...\n");
    testColumnEditor();
    std::printf("style catalog save...\n");
    testStyleCatalogSave();
    std::printf("user defined languages...\n");
    testUserLanguages();

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
