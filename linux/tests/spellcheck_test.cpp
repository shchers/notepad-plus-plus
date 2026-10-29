// The bundled Spell Checker plugin's word finding and style choice, and its
// Enchant dictionaries when Enchant and an English dictionary are installed.
// No display needed.

#include "SpellText.h"
#include "Speller.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace spellcheck;

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

// The words found, joined by '|'.
std::string words(const std::string& text, const WordOptions& o = {}, const std::string& checkable = {})
{
    std::string out;
    for (const Word& w : findWords(text, checkable, o)) {
        if (!out.empty())
            out += '|';
        out += w.text;
    }
    return out;
}

void testWords()
{
    CHECK(words("Hello, wrold! It's a test.") == "Hello|wrold|It's|test");
    CHECK(words("well-known co-operation") == "well|known|co|operation");
    // Typographic apostrophes are checked as ASCII ones; quotes are not words.
    CHECK(words("don\u2019t 'quoted' rock'n'roll") == "don't|quoted|rock'n'roll");
    // Offsets are bytes, also after other scripts.
    {
        const std::string text = "Größe naïve слово";
        const std::vector<Word> w = findWords(text, {}, {});
        CHECK(w.size() == 3);
        CHECK(w.size() == 3 && text.substr(w[2].start, w[2].end - w[2].start) == "слово");
        CHECK(w.size() == 3 && w[1].text == "naïve");
    }
    // Left out: one letter, '_', digits, capitals, camelCase.
    CHECK(words("a I x") == "");
    CHECK(words("snake_case utf8 NASA camelCase iPhone Word") == "Word");
    WordOptions keep;
    keep.ignoreDigits = keep.ignoreAllCaps = keep.ignoreMixedCase = false;
    CHECK(words("snake_case utf8 NASA camelCase", keep) == "utf8|NASA|camelCase");
    // URLs, e-mail addresses, paths, dotted names, calls, variables, escapes.
    CHECK(words("see https://exmaple.com/pth and www.exmaple.org") == "see|and");
    CHECK(words("mail jonh@exmaple.com now") == "mail|now");
    CHECK(words("open /usr/shrae/doc or C:\\Usres\\me") == "open|or");
    CHECK(words("edit config.yaml, then stop.") == "edit|then|stop");
    CHECK(words("call fooo() and std::vectr here") == "call|and|here");
    CHECK(words("print $varr and %s or &amp; \\nnext #tagg") == "print|and|or");
    CHECK(words("key=valeu") == "");
    // Only checkable bytes: a word partly outside is not a word to check.
    {
        const std::string text = "int x; // a commnet";
        std::string mask(text.size(), '\0');
        std::fill(mask.begin() + static_cast<long>(text.find("//")), mask.end(), '\1');
        CHECK(words(text, {}, mask) == "commnet");
        std::string half(text.size(), '\1');
        half[text.find("commnet") + 3] = '\0';
        CHECK(words(text, {}, half) == "int");
    }
    // Invalid UTF-8 separates words.
    CHECK(words(std::string("abc\xff" "def")) == "abc|def");
}

void testStyles()
{
    StyleQuery cpp;
    cpp.lexer = "cpp";
    cpp.tags = {"default", "comment", "comment line", "comment documentation", "literal numeric", "keyword", "literal string",
                "literal string character", "", "preprocessor", "operator", "identifier", "literal string", "", "literal regex",
                "comment line documentation", "", "comment documentation keyword", "comment documentation keyword error"};
    auto s = checkableStyles(cpp);
    CHECK(!s[0] && s[1] && s[2] && s[3] && !s[4] && !s[5] && s[6] && s[7] && !s[9] && !s[10] && !s[11] && !s[14] && s[15] && !s[17] && !s[18]);

    StyleQuery html;
    html.lexer = "hypertext";
    html.tags = {"default", "tag", "error tag", "attribute", "error attribute", "literal numeric", "literal string", "literal string",
                 "tag operator", "comment"};
    s = checkableStyles(html);
    CHECK(s[0] && !s[1] && !s[3] && !s[6] && !s[7] && s[9]);

    StyleQuery text;
    text.lexer = "null";
    s = checkableStyles(text);
    CHECK(s[0] && s[255]);

    StyleQuery md;
    md.lexer = "markdown";
    s = checkableStyles(md);
    CHECK(s[0] && s[6] && !s[18] && !s[19] && !s[21]);

    StyleQuery udl;
    udl.lexer = "user";
    s = checkableStyles(udl);
    CHECK(!s[0] && s[1] && s[2] && s[16] && s[17] && !s[4]);
    udl.prose = true;
    udl.markdown = true;
    s = checkableStyles(udl);
    CHECK(s[0] && s[4] && !s[17]);

    StyleQuery sql;
    sql.lexer = "sql";
    s = checkableStyles(sql);
    CHECK(s[1] && s[6] && !s[0] && !s[5]);
    sql.prose = true; // "check only comments and strings" off
    s = checkableStyles(sql);
    CHECK(s[0] && s[5]);

    StyleQuery unknown;
    unknown.lexer = "asn1";
    s = checkableStyles(unknown);
    CHECK(std::none_of(s.begin(), s.end(), [](bool b) { return b; }));

    CHECK(isProseFile("/home/u/notes.TXT"));
    CHECK(isProseFile("README.md"));
    CHECK(!isProseFile("main.cpp"));
    CHECK(!isProseFile("/a.md/Makefile"));
}

void testSpeller(const std::string& tmp)
{
    Speller sp;
    if (!sp.available()) {
        std::printf("SKIP dictionaries: %s\n", sp.error().c_str());
        return;
    }
    const std::vector<std::string> dicts = sp.dictionaries();
    if (std::find(dicts.begin(), dicts.end(), "en_US") == dicts.end()) {
        std::printf("SKIP dictionaries: no en_US dictionary (hunspell-en-us)\n");
        return;
    }
    // Nothing chosen: nothing is wrong.
    CHECK(sp.correct("wrold"));
    sp.setLanguages({"xx_NOPE", "en_US"});
    CHECK(sp.languages() == std::vector<std::string>{"en_US"});
    CHECK(sp.correct("world"));
    CHECK(sp.correct("Hello"));
    CHECK(sp.correct("don't"));
    CHECK(!sp.correct("wrold"));
    const std::vector<std::string> s = sp.suggest("wrold", 5);
    CHECK(!s.empty() && s.size() <= 5);
    CHECK(std::find(s.begin(), s.end(), "world") != s.end());

    sp.ignore("wrold");
    CHECK(sp.correct("wrold"));

    const std::string dic = tmp + "/sub/personal.dic";
    sp.setPersonalDictionary(dic);
    CHECK(g_file_test(dic.c_str(), G_FILE_TEST_EXISTS));
    CHECK(!sp.correct("Shcherbakov"));
    sp.add("Shcherbakov");
    CHECK(sp.correct("Shcherbakov"));
    {
        std::ifstream f(dic);
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(ss.str().find("Shcherbakov") != std::string::npos);
    }
    // Edited outside: read again.
    {
        std::ofstream f(dic, std::ios::app);
        f << "Notepadplusplus\n";
    }
    CHECK(!sp.correct("Notepadplusplus"));
    sp.reloadPersonalDictionary();
    CHECK(sp.correct("Notepadplusplus"));
    CHECK(sp.correct("Shcherbakov"));
}

} // namespace

int main()
{
    char tmpl[] = "/tmp/nppgtk-spell-XXXXXX";
    const char* tmp = g_mkdtemp(tmpl);
    if (!tmp)
        return 2;
    testWords();
    testStyles();
    testSpeller(tmp);
    const std::string cmd = std::string("rm -rf '") + tmp + "'";
    if (std::system(cmd.c_str()) != 0)
        std::fprintf(stderr, "cannot remove %s\n", tmp);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
