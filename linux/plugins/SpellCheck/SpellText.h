// SpellCheck — which words of a text to check, and which styles of a lexer
// hold prose. No Notepad++ or Enchant here, so the tests use it directly.
#pragma once

#include <glib.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spellcheck {

struct WordOptions {
    bool ignoreDigits = true;    // r2d2, utf8
    bool ignoreAllCaps = true;   // NASA, HTTP
    bool ignoreMixedCase = true; // camelCase, iPhone, McDonald
};

struct Word {
    std::size_t start = 0, end = 0; // byte offsets in the text
    std::string text;               // as the dictionary wants it: ’ as '
    std::string lang;               // a misspelling: the language it was checked in
};

// The words of `text` (UTF-8) worth checking. `checkable` is empty (all of
// it) or one flag per byte of text: a word must lie in checkable bytes.
// Left out: words with '_', of one letter, part of a URL, an e-mail
// address, a path or a dotted name (file.txt), a call (name(), a::b), a
// variable ($x, %s) or an escape (\n), and what the options say.
std::vector<Word> findWords(std::string_view text, std::string_view checkable, const WordOptions& o);

// Which styles of a document to check, from what the editor tells about it.
struct StyleQuery {
    std::string lexer;              // SCI_GETLEXERLANGUAGE ("cpp", "user", "null")
    std::vector<std::string> tags;  // SCI_TAGSOFSTYLE for each named style
    bool prose = false;             // plain text, Markdown, LaTeX...: all text
    bool markdown = false;          // the Markdown UDL: not its code spans
};
std::array<bool, 256> checkableStyles(const StyleQuery& q);

// Where paragraphs (text between blank lines) begin: indexes into words,
// the first always 0. Each paragraph gets its own language.
std::vector<std::size_t> paragraphStarts(std::string_view text, const std::vector<Word>& words);

// The script a word is written in (its first letter's): Latin, Cyrillic...
GUnicodeScript wordScript(std::string_view word);
// The script of a language's dictionary, from its tag ("ru_RU": Cyrillic).
GUnicodeScript languageScript(const std::string& tag);
// A language tag ("de", "de_DE", "sr_Latn_RS"), not an Aspell variant
// ("en-variant_0", "en_GB-ize-w_accents").
bool isLanguageTag(const std::string& tag);
// For language detection: one dictionary per language from the installed
// ones, the user's own languages (`preferred`, best first: "de_AT", "de")
// first. A language's variant is the preferred one, else the main country's
// (de_DE, fr_FR, en_US), else the first.
std::vector<std::string> detectionLanguages(const std::vector<std::string>& installed, const std::vector<std::string>& preferred);

// A document read as text: by language (Normal Text) or file extension.
bool isProseFile(const std::string& path);

} // namespace spellcheck
