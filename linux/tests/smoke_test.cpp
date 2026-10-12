// Smoke tests for the port: data loading, encodings, and a live Scintilla
// widget with Lexilla lexing and the Boost regex engine.
// Plain asserts + exit code keep this free of test-framework dependencies.

#include "core/LanguageCatalog.h"
#include "core/StyleCatalog.h"
#include "platform/TextCodec.h"
#include "ui/EditorView.h"
#include "ui/FindBar.h"

#include <SciLexer.h>
#include <gtk/gtk.h>

#include <cstdio>
#include <filesystem>
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

static void testLanguages(const core::LanguageCatalog& langs)
{
    CHECK(langs.all().size() > 80);
    CHECK(langs.forFile("main.cpp").name == "cpp");
    CHECK(langs.forFile("x/y/Script.PY").name == "python");
    CHECK(langs.forFile("Makefile").name == "makefile");
    CHECK(langs.forFile("CMakeLists.txt").name == "cmake");
    CHECK(langs.forFile(".bashrc").name == "bash");
    CHECK(langs.forFile("README").isPlainText());
    CHECK(langs.forFile("notes.txt").isPlainText());
    const core::Language* cpp = langs.byName("cpp");
    CHECK(cpp && cpp->lexer == "cpp");
    CHECK(cpp && (cpp->keywords[0].find("constexpr") != std::string::npos || cpp->keywords[2].find("constexpr") != std::string::npos));
    CHECK(cpp && cpp->commentLine == "//");
    CHECK(langs.byAnyName("C++") == cpp);
    CHECK(langs.byAnyName("PYTHON") == langs.byName("python"));
}

static void testStyles(const core::StyleCatalog& styles)
{
    const core::Style* def = styles.globalById(STYLE_DEFAULT);
    CHECK(def && def->fontSize.value_or(0) > 0);
    CHECK(styles.forLanguage("cpp") && styles.forLanguage("cpp")->size() > 10);
    CHECK(core::StyleCatalog::parseColour("FF8000") == 0x0080FFu); // RGB -> BGR
    CHECK(!core::StyleCatalog::parseColour("xyz"));
}

static void testCodec()
{
    using platform::Bom;
    auto d = platform::decode("hello \xC3\xA9");
    CHECK(d.encoding.charset == "UTF-8" && d.encoding.bom == Bom::None && d.utf8 == "hello \xC3\xA9");

    d = platform::decode("\xEF\xBB\xBFx");
    CHECK(d.encoding.bom == Bom::Utf8 && d.utf8 == "x" && d.encoding.displayName() == "UTF-8-BOM");
    CHECK(platform::encode("x", d.encoding).value() == "\xEF\xBB\xBFx");

    const std::string utf16le("\xFF\xFE" "a\0b\0", 6);
    d = platform::decode(utf16le);
    CHECK(d.encoding.bom == Bom::Utf16LE && d.utf8 == "ab");
    CHECK(platform::encode(d.utf8, d.encoding).value() == utf16le);

    // Latin-1 text must round-trip byte-exact.
    const std::string latin = "Caf\xE9 cr\xE8me br\xFBl\xE9" "e, na\xEFve fa\xE7" "ade, d\xE9j\xE0 vu, \xE0 la carte.\n";
    d = platform::decode(latin);
    CHECK(d.encoding.charset != "UTF-8");
    CHECK(d.utf8.find("\xC3\xA9") != std::string::npos); // é decoded
    CHECK(platform::encode(d.utf8, d.encoding).value_or("") == latin);
    std::printf("  latin-1 sample detected as %s\n", d.encoding.displayName().c_str());

    // Non-representable characters must fail cleanly, not silently corrupt.
    std::string err;
    CHECK(!platform::encode("\xE2\x82\xAC\xE4\xB8\xAD", {"ISO-8859-1", Bom::None}, &err));
    CHECK(!err.empty());
}

static void testEditor(const core::LanguageCatalog& langs, const core::StyleCatalog& styles)
{
    GtkWidget* win = gtk_offscreen_window_new();
    ui::EditorView view;
    gtk_container_add(GTK_CONTAINER(win), view.widget());
    gtk_widget_show_all(win);

    view.setText("int main() {\n\treturn 42; // answer\n}\n");
    CHECK(view.lineCount() == 4);
    CHECK(!view.isModified());

    view.applyLanguage(*langs.byName("cpp"), langs, styles);
    // "int" is a type keyword -> SCE_C_WORD2, "return" an instruction keyword -> SCE_C_WORD.
    CHECK(view.execute(SCI_GETSTYLEAT, 0) == SCE_C_WORD2);
    const sptr_t ret = static_cast<sptr_t>(view.text().find("return"));
    CHECK(view.execute(SCI_GETSTYLEAT, static_cast<uptr_t>(ret)) == SCE_C_WORD);
    const sptr_t cmt = static_cast<sptr_t>(view.text().find("//"));
    CHECK(view.execute(SCI_GETSTYLEAT, static_cast<uptr_t>(cmt)) == SCE_C_COMMENTLINE);
    // Keyword styles carry upstream colours: "INSTRUCTION WORD" is 0000FF (blue).
    CHECK(view.execute(SCI_STYLEGETFORE, SCE_C_WORD) == 0xFF0000);

    // Hypertext family: embedded JS keywords come from the "javascript" language.
    {
        ui::EditorView html;
        html.setText("<p>x</p><script>var a = 1;</script>");
        html.applyLanguage(*langs.byName("html"), langs, styles);
        CHECK(html.execute(SCI_GETSTYLEAT, 1) == SCE_H_TAG);
        const sptr_t var = static_cast<sptr_t>(html.text().find("var"));
        CHECK(html.execute(SCI_GETSTYLEAT, static_cast<uptr_t>(var)) == SCE_HJ_KEYWORD);
        // Identity-mapped lexer: Python instre1 -> keyword set 0.
        ui::EditorView py;
        py.setText("def f():\n    return None\n");
        py.applyLanguage(*langs.byName("python"), langs, styles);
        CHECK(py.execute(SCI_GETSTYLEAT, 0) == SCE_P_WORD);
    }

    // Boost regex (lookbehind is unsupported by Scintilla's built-in engine and std::regex).
    ui::SearchOptions o;
    o.regex = true;
    o.pattern = "(?<=return )\\d+";
    view.execute(SCI_GOTOPOS, 0);
    CHECK(ui::findNext(view, o, true) == ui::SearchResult::Found);
    CHECK(view.selectedText() == "42");
    o.pattern = "(unclosed";
    CHECK(ui::findNext(view, o, true) == ui::SearchResult::InvalidRegex);

    // Regex replace with backreference, single undo step. Boost format_all
    // syntax as on Windows Notepad++: literal parentheses are escaped.
    o.pattern = "(\\w+)\\(\\)";
    CHECK(ui::replaceAll(view, o, "\\1\\(void\\)") == 1);
    CHECK(view.text().find("main(void)") != std::string::npos);
    view.execute(SCI_UNDO);
    CHECK(view.text().find("main()") != std::string::npos);

    // Empty matches must terminate.
    o.pattern = "^";
    // Notepad++ semantics: "^" matches the start of each of the 4 lines
    // (including the empty last line).
    CHECK(ui::replaceAll(view, o, "> ") == 4);
    CHECK(view.text().rfind("> int main", 0) == 0);
    view.execute(SCI_UNDO);

    // Plain search wraps around.
    ui::SearchOptions plain;
    plain.pattern = "int";
    view.execute(SCI_GOTOPOS, static_cast<uptr_t>(view.length()));
    CHECK(ui::findNext(view, plain, true) == ui::SearchResult::Wrapped);

    // Toggle comment uses langs.model.xml commentLine and round-trips.
    view.execute(SCI_SETSEL, 0, view.length());
    const std::string before = view.text();
    view.toggleLineComment();
    CHECK(view.text().rfind("// int main", 0) == 0);
    view.execute(SCI_SETSEL, 0, view.length());
    view.toggleLineComment();
    CHECK(view.text() == before);

    // Auto-indent: typing Enter on an indented line keeps the indentation.
    view.setText("\tif (x) {");
    view.execute(SCI_DOCUMENTEND);
    view.execute(SCI_NEWLINE); // same path as the Enter key, emits SCN_CHARADDED
    CHECK(view.text() == "\tif (x) {\n\t");
    CHECK(view.currentPos() == view.length());

    // EOL detection and bookmarks.
    view.setText("a\r\nb\r\n");
    CHECK(view.detectEolMode() == SC_EOL_CRLF);
    view.toggleBookmark(1);
    view.execute(SCI_GOTOPOS, 0);
    CHECK(view.gotoNextBookmark(true));
    CHECK(view.lineFromPos(view.currentPos()) == 1);

    gtk_widget_destroy(win);
}

int main(int argc, char** argv)
{
    gtk_init(&argc, &argv);
    const fs::path data = NPP_TEST_DATA_DIR;

    core::LanguageCatalog langs;
    std::string err;
    CHECK(langs.load(data / "langs.model.xml", &err));
    core::StyleCatalog styles;
    CHECK(styles.load(data / "stylers.model.xml", &err));
    if (!err.empty())
        std::fprintf(stderr, "load error: %s\n", err.c_str());

    std::printf("languages...\n");
    testLanguages(langs);
    std::printf("styles...\n");
    testStyles(styles);
    std::printf("codec...\n");
    testCodec();
    std::printf("editor widget...\n");
    testEditor(langs, styles);

    // Every bundled theme must parse.
    int themes = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(data / "themes", ec)) {
        core::StyleCatalog t;
        CHECK(t.load(e.path(), &err));
        ++themes;
    }
    std::printf("themes parsed: %d\n", themes);
    CHECK(themes > 10);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
