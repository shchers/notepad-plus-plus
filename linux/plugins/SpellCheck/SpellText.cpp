// SpellCheck — words and styles; see SpellText.h.

#include "SpellText.h"

#include <glib.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace spellcheck {

namespace {

constexpr gunichar kRightQuote = 0x2019; // ’, typographic apostrophe

bool isApostrophe(gunichar c)
{
    return c == '\'' || c == kRightQuote;
}

bool isWordChar(gunichar c)
{
    return g_unichar_isalnum(c) || g_unichar_ismark(c) || c == '_' || isApostrophe(c);
}

// The text as code points with their byte offsets; an invalid byte is one
// U+FFFD, which no word contains.
struct Decoded {
    std::vector<gunichar> chars;
    std::vector<std::size_t> offsets; // one more than chars: the end
};

Decoded decode(std::string_view text)
{
    Decoded d;
    d.chars.reserve(text.size());
    d.offsets.reserve(text.size() + 1);
    std::size_t i = 0;
    while (i < text.size()) {
        d.offsets.push_back(i);
        const gunichar c = g_utf8_get_char_validated(text.data() + i, static_cast<gssize>(text.size() - i));
        if (c == static_cast<gunichar>(-1) || c == static_cast<gunichar>(-2)) {
            d.chars.push_back(0xFFFD);
            ++i;
        } else {
            d.chars.push_back(c);
            i += static_cast<std::size_t>(g_utf8_skip[static_cast<unsigned char>(text[i])]);
        }
    }
    d.offsets.push_back(text.size());
    return d;
}

bool checkableRange(std::string_view checkable, std::size_t from, std::size_t to)
{
    if (checkable.empty())
        return true;
    for (std::size_t i = from; i < to && i < checkable.size(); ++i)
        if (!checkable[i])
            return false;
    return true;
}

} // namespace

std::vector<Word> findWords(std::string_view text, std::string_view checkable, const WordOptions& o)
{
    std::vector<Word> words;
    const Decoded d = decode(text);
    const std::vector<gunichar>& c = d.chars;
    const std::size_t n = c.size();
    auto at = [&](std::size_t i, std::ptrdiff_t delta) -> gunichar {
        const std::ptrdiff_t j = static_cast<std::ptrdiff_t>(i) + delta;
        return j < 0 || j >= static_cast<std::ptrdiff_t>(n) ? 0 : c[static_cast<std::size_t>(j)];
    };

    // Chunks between blanks that hold a URL or an e-mail address are left
    // out whole.
    std::vector<bool> skipChunk(n, false);
    for (std::size_t i = 0; i < n;) {
        if (g_unichar_isspace(c[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        std::string chunk;
        while (j < n && !g_unichar_isspace(c[j])) {
            if (c[j] < 0x80)
                chunk += static_cast<char>(c[j]);
            ++j;
        }
        if (chunk.find("://") != std::string::npos || chunk.find("www.") != std::string::npos || chunk.find('@') != std::string::npos)
            std::fill(skipChunk.begin() + static_cast<std::ptrdiff_t>(i), skipChunk.begin() + static_cast<std::ptrdiff_t>(j), true);
        i = j;
    }

    for (std::size_t i = 0; i < n;) {
        if (!isWordChar(c[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < n && isWordChar(c[end]))
            ++end;
        std::size_t s = i, e = end;
        i = end;
        while (s < e && isApostrophe(c[s]))
            ++s;
        while (e > s && isApostrophe(c[e - 1]))
            --e;
        if (e - s < 2 || e - s > 64 || skipChunk[s])
            continue;
        if (!checkableRange(checkable, d.offsets[s], d.offsets[e]))
            continue;

        // What surrounds it: a path, a dotted name, a call, a variable.
        const gunichar before = at(s, -1), after = at(e - 1, 1);
        if (before && before < 0x80 && std::strchr("/\\@$%&#~=", static_cast<int>(before)))
            continue;
        if (after && after < 0x80 && std::strchr("/\\@(=", static_cast<int>(after)))
            continue;
        if (before == '.' && g_unichar_isalnum(at(s, -2)))
            continue;
        if (after == '.' && g_unichar_isalnum(at(e - 1, 2)))
            continue;
        if ((before == ':' && at(s, -2) == ':') || (after == ':' && at(e - 1, 2) == ':'))
            continue;

        bool digit = false, underscore = false;
        int letters = 0, upper = 0, upperInside = 0;
        for (std::size_t k = s; k < e; ++k) {
            if (c[k] == '_')
                underscore = true;
            else if (g_unichar_isdigit(c[k]))
                digit = true;
            else if (g_unichar_isalpha(c[k])) {
                ++letters;
                if (g_unichar_isupper(c[k]) || g_unichar_istitle(c[k])) {
                    ++upper;
                    if (k > s)
                        ++upperInside;
                }
            }
        }
        if (underscore || letters == 0 || (digit && o.ignoreDigits))
            continue;
        const bool allCaps = upper == letters;
        if (allCaps && o.ignoreAllCaps)
            continue;
        if (!allCaps && upperInside > 0 && o.ignoreMixedCase)
            continue;

        Word w;
        w.start = d.offsets[s];
        w.end = d.offsets[e];
        for (std::size_t k = s; k < e; ++k) {
            if (c[k] == kRightQuote) {
                w.text += '\'';
            } else {
                char buf[6];
                w.text.append(buf, static_cast<std::size_t>(g_unichar_to_utf8(c[k], buf)));
            }
        }
        words.push_back(std::move(w));
    }
    return words;
}

std::array<bool, 256> checkableStyles(const StyleQuery& q)
{
    std::array<bool, 256> on{};
    auto all = [&] { on.fill(true); };
    auto some = [&](std::initializer_list<int> styles) {
        for (int s : styles)
            on[static_cast<std::size_t>(s)] = true;
    };

    if (q.lexer.empty() || q.lexer == "null") {
        all();
        return on;
    }
    // Lexers of text: never their code, commands or links.
    if (q.lexer == "markdown") {
        all();
        for (int s = 18; s <= 21; ++s) // SCE_MARKDOWN_LINK, _CODE, _CODE2, _CODEBK
            on[static_cast<std::size_t>(s)] = false;
        return on;
    }
    if (q.lexer == "latex") {
        some({0, 4, 7}); // SCE_L_DEFAULT, _COMMENT, _COMMENT2
        return on;
    }
    if (q.lexer == "tex") {
        some({0, 5}); // SCE_TEX_DEFAULT, _TEXT
        return on;
    }
    if (q.lexer == "user") {
        // User Defined Languages: comments and delimited text (strings), or
        // all of it for a text; the Markdown UDL's DELIMITERS2 is code.
        if (q.prose)
            all();
        else
            some({1, 2, 15, 16, 17, 18, 19, 20, 21, 22, 23});
        if (q.markdown)
            on[17] = false;
        return on;
    }
    if (q.prose) {
        all();
        return on;
    }

    // Lexers that tell what their styles are: comments and strings, and
    // the text of HTML and XML (not their attribute values).
    if (!q.tags.empty()) {
        const bool markup = q.lexer == "hypertext" || q.lexer == "xml" || q.lexer == "phpscript";
        for (std::size_t s = 0; s < q.tags.size() && s < on.size(); ++s) {
            std::set<std::string> t;
            gchar** parts = g_strsplit(q.tags[s].c_str(), " ", -1);
            for (gchar** p = parts; *p; ++p)
                t.insert(*p);
            g_strfreev(parts);
            if (t.count("keyword") || t.count("identifier") || t.count("operator") || t.count("tag") || t.count("regex"))
                continue;
            if (t.count("comment") || (t.count("string") && !(markup && s <= 7)) || (markup && s == 0))
                on[s] = true;
        }
        return on;
    }

    // Common lexers without that: their comment and string styles.
    static const std::map<std::string, std::vector<int>> known = {
        {"sql", {1, 2, 3, 6, 7, 15}},             // SCE_SQL_COMMENT.., _STRING, _CHARACTER, _COMMENTLINEDOC
        {"css", {9, 13, 14}},                     // SCE_CSS_COMMENT, _DOUBLESTRING, _SINGLESTRING
        {"props", {1}},                           // SCE_PROPS_COMMENT (INI files)
        {"yaml", {1, 7}},                         // SCE_YAML_COMMENT, _TEXT
        {"json", {2, 6, 7}},                      // SCE_JSON_STRING, _LINECOMMENT, _BLOCKCOMMENT
        {"powershell", {1, 2, 3, 13, 14, 15}},    // SCE_POWERSHELL_COMMENT, strings, here-strings
        {"r", {1, 6, 7}},                         // SCE_R_COMMENT, _STRING, _STRING2
        {"fortran", {1, 3, 4}},                   // SCE_F_COMMENT, _STRING1, _STRING2
        {"f77", {1, 3, 4}},
        {"lisp", {1, 6, 12}},                     // SCE_LISP_COMMENT, _STRING, _MULTI_COMMENT
        {"matlab", {1, 5, 8}},                    // SCE_MATLAB_COMMENT, _STRING, _DOUBLEQUOTESTRING
        {"octave", {1, 5, 8}},
    };
    const auto it = known.find(q.lexer);
    if (it != known.end())
        for (int s : it->second)
            on[static_cast<std::size_t>(s)] = true;
    return on;
}

bool isProseFile(const std::string& path)
{
    static const std::set<std::string> prose = {"txt", "text", "md", "markdown", "mdown", "mkd", "rst", "adoc",
                                                "asciidoc", "org", "tex", "ltx", "textile", "wiki"};
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(g_ascii_tolower(ch)); });
    return prose.count(ext) != 0;
}

} // namespace spellcheck
