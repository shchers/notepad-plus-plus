#include "core/EditorFeatures.h"

#include <SciLexer.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stack>

#include <strings.h>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

char charAt(const Sci& sci, sptr_t pos)
{
    return static_cast<char>(sci(SCI_GETCHARAT, static_cast<uptr_t>(pos)));
}

// SCI_SEARCHINTARGET between `from` and `to` (backwards when from > to).
sptr_t searchIn(const Sci& sci, std::string_view what, sptr_t from, sptr_t to, int flags)
{
    sci(SCI_SETSEARCHFLAGS, static_cast<uptr_t>(flags));
    sci.setTarget(from, to);
    return sci(SCI_SEARCHINTARGET, what.size(), reinterpret_cast<sptr_t>(what.data()));
}

void fill(const Sci& sci, int indicator, sptr_t start, sptr_t len)
{
    if (len <= 0)
        return;
    sci(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator));
    sci(SCI_INDICATORFILLRANGE, static_cast<uptr_t>(start), len);
}

// First and last document line on screen.
std::pair<sptr_t, sptr_t> visibleLines(const Sci& sci)
{
    const sptr_t firstVisible = sci(SCI_GETFIRSTVISIBLELINE);
    const sptr_t onScreen = std::min<sptr_t>(sci(SCI_LINESONSCREEN), 400) + 1;
    const sptr_t first = sci(SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>(firstVisible));
    const sptr_t last = std::min(sci(SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>(firstVisible + onScreen)), sci.lineCount() - 1);
    return {first, last};
}

std::string regexEscape(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (std::strchr("\\^$.|?*+()[]{}", c))
            out += '\\';
        out += c;
    }
    return out;
}

} // namespace

void clearIndicator(const Sci& sci, int indicator)
{
    sci(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator));
    sci(SCI_INDICATORCLEARRANGE, 0, sci.length());
}

// ============================================================================
// Smart highlighting
// ============================================================================

namespace {

void highlightWordIn(const Sci& v, const std::string& word, bool wholeWord, bool matchCase)
{
    const sptr_t tStart = v(SCI_GETTARGETSTART), tEnd = v(SCI_GETTARGETEND);
    const int flags = (matchCase ? SCFIND_MATCHCASE : 0) | (wholeWord ? SCFIND_WHOLEWORD : 0);
    const auto [first, last] = visibleLines(v);
    const sptr_t from = v.lineStart(first);
    const sptr_t to = last + 1 < v.lineCount() ? v.lineStart(last + 1) : v.length();
    sptr_t pos = from;
    while (pos < to) {
        const sptr_t found = searchIn(v, word, pos, to, flags);
        if (found < 0)
            break;
        const sptr_t end = v(SCI_GETTARGETEND);
        fill(v, kSmartIndicator, found, end - found);
        pos = end > found ? end : found + 1;
    }
    v.setTarget(tStart, tEnd);
}

} // namespace

void smartHighlight(const Sci& view, const Sci* other, bool wholeWord, bool matchCase)
{
    clearIndicator(view, kSmartIndicator);
    const bool sameDoc = other && (*other)(SCI_GETDOCPOINTER) == view(SCI_GETDOCPOINTER);
    if (view(SCI_GETSELECTIONEMPTY)) {
        if (other && !sameDoc)
            clearIndicator(*other, kSmartIndicator);
        return;
    }
    const sptr_t curPos = view(SCI_GETCURRENTPOS);
    const sptr_t selStart = view(SCI_GETSELECTIONSTART), selEnd = view(SCI_GETSELECTIONEND);
    if (wholeWord) {
        const sptr_t ws = view(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(curPos), true);
        const sptr_t we = view(SCI_WORDENDPOSITION, static_cast<uptr_t>(ws), true);
        if (ws == we || ws != selStart || we != selEnd)
            return;
    } else if (selEnd - selStart > view(SCI_LINELENGTH, static_cast<uptr_t>(view.lineOf(curPos)))) {
        return;
    }
    const std::string word = view.range(selStart, selEnd);
    if (word.empty())
        return;
    highlightWordIn(view, word, wholeWord, matchCase);
    if (other) {
        if (!sameDoc)
            clearIndicator(*other, kSmartIndicator);
        highlightWordIn(*other, word, wholeWord, matchCase);
    }
}

// ============================================================================
// Matching tags
// ============================================================================

namespace {

bool isNameChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.' || c == '-' || static_cast<unsigned char>(c) >= 0x80;
}

struct Tag {
    sptr_t lt = -1, gt = -1; // '<' and '>'
    sptr_t nameStart = 0;
    std::string name;
    bool closing = false;
    bool selfClosing = false;
};

// The '>' of a tag starting at `lt`, skipping quoted attribute values; -1 if none.
sptr_t tagEnd(const Sci& sci, sptr_t lt)
{
    const sptr_t len = sci.length();
    char quote = 0;
    for (sptr_t p = lt + 1; p < len; ++p) {
        const char c = charAt(sci, p);
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return p;
        } else if (c == '<') {
            return -1;
        }
    }
    return -1;
}

bool parseTag(const Sci& sci, sptr_t lt, Tag& t)
{
    t.lt = lt;
    t.gt = tagEnd(sci, lt);
    if (t.gt < 0)
        return false;
    sptr_t p = lt + 1;
    t.closing = charAt(sci, p) == '/';
    if (t.closing)
        ++p;
    t.nameStart = p;
    while (p < t.gt && isNameChar(charAt(sci, p)))
        ++p;
    t.name = sci.range(t.nameStart, p);
    t.selfClosing = !t.closing && charAt(sci, t.gt - 1) == '/';
    return !t.name.empty();
}

// The tag around `pos`: from its '<' to just after its '>'.
bool tagAround(const Sci& sci, sptr_t pos, Tag& t)
{
    sptr_t lt = -1;
    if (pos < sci.length() && charAt(sci, pos) == '<') {
        lt = pos;
    } else {
        for (sptr_t p = pos - 1; p >= 0 && pos - p < 4096; --p) {
            const char c = charAt(sci, p);
            if (c == '<') {
                lt = p;
                break;
            }
            if (c == '>' && p != pos - 1)
                return false;
        }
    }
    if (lt < 0 || !parseTag(sci, lt, t))
        return false;
    return pos <= t.gt + 1;
}

// The tag `t` pairs with.
bool partnerTag(const Sci& sci, const Tag& t, bool ignoreCase, Tag& out)
{
    const std::string pattern = "</?" + regexEscape(t.name) + "[ \t\r\n/>]";
    const int flags = SCFIND_REGEXP | SCFIND_POSIX | (ignoreCase ? 0 : SCFIND_MATCHCASE);
    int depth = 0;
    if (!t.closing) {
        sptr_t pos = t.gt + 1;
        const sptr_t len = sci.length();
        while (pos < len) {
            const sptr_t m = searchIn(sci, pattern, pos, len, flags);
            if (m < 0)
                return false;
            Tag c;
            pos = m + 1;
            if (!parseTag(sci, m, c))
                continue;
            if (c.closing) {
                if (depth == 0) {
                    out = c;
                    return true;
                }
                --depth;
            } else if (!c.selfClosing) {
                ++depth;
            }
        }
        return false;
    }
    sptr_t pos = t.lt;
    while (pos > 0) {
        const sptr_t m = searchIn(sci, pattern, pos, 0, flags);
        if (m < 0)
            return false;
        Tag c;
        pos = m;
        if (!parseTag(sci, m, c))
            continue;
        if (c.closing) {
            ++depth;
        } else if (!c.selfClosing) {
            if (depth == 0) {
                out = c;
                return true;
            }
            --depth;
        }
    }
    return false;
}

// Caret inside <!-- -->, <? ?> or <% %>: marks both delimiters.
bool markZone(const Sci& sci, sptr_t pos)
{
    static const std::pair<std::string_view, std::string_view> kZones[] = {{"<!--", "-->"}, {"<?", "?>"}, {"<%", "%>"}};
    for (const auto& [open, close] : kZones) {
        const auto ol = static_cast<sptr_t>(open.size()), cl = static_cast<sptr_t>(close.size());
        const sptr_t o = searchIn(sci, open, std::min(pos + ol - 1, sci.length()), 0, SCFIND_MATCHCASE);
        if (o < 0)
            continue;
        const sptr_t c = searchIn(sci, close, o + ol, sci.length(), SCFIND_MATCHCASE);
        if (c < 0 || c + cl < pos)
            continue; // closed before the caret
        fill(sci, kTagMatchIndicator, o, ol);
        fill(sci, kTagMatchIndicator, c, cl);
        return true;
    }
    return false;
}

void markAttributes(const Sci& sci, const Tag& t)
{
    sptr_t p = t.nameStart + static_cast<sptr_t>(t.name.size());
    while (p < t.gt) {
        const char c = charAt(sci, p);
        if (c == '"' || c == '\'') { // a value: skip it
            sptr_t q = p + 1;
            while (q < t.gt && charAt(sci, q) != c)
                ++q;
            p = q + 1;
            continue;
        }
        if (!isNameChar(c)) {
            ++p;
            continue;
        }
        const sptr_t start = p;
        while (p < t.gt && isNameChar(charAt(sci, p)))
            ++p;
        sptr_t q = p;
        while (q < t.gt && std::isspace(static_cast<unsigned char>(charAt(sci, q))))
            ++q;
        if (q < t.gt && charAt(sci, q) == '=')
            fill(sci, kTagAttrIndicator, start, p - start);
    }
}

} // namespace

void highlightMatchingTags(const Sci& sci, bool attributes, bool zones, bool ignoreCase)
{
    const sptr_t tStart = sci(SCI_GETTARGETSTART), tEnd = sci(SCI_GETTARGETEND);
    clearIndicator(sci, kTagMatchIndicator);
    clearIndicator(sci, kTagAttrIndicator);
    const sptr_t pos = sci(SCI_GETCURRENTPOS);
    Tag t;
    if (tagAround(sci, pos, t)) {
        Tag partner;
        if (!t.selfClosing && partnerTag(sci, t, ignoreCase, partner)) {
            fill(sci, kTagMatchIndicator, t.nameStart, static_cast<sptr_t>(t.name.size()));
            fill(sci, kTagMatchIndicator, partner.nameStart, static_cast<sptr_t>(partner.name.size()));
        }
        if (attributes && !t.closing)
            markAttributes(sci, t);
    } else if (zones) {
        markZone(sci, pos);
    }
    sci.setTarget(tStart, tEnd);
}

// ============================================================================
// Clickable links (upstream isUrl, on code points)
// ============================================================================

namespace {

bool isSchemeStartChar(char32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool isSchemeDelimiter(char32_t c)
{
    return !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_');
}

bool isUrlTextChar(char32_t c)
{
    if (c <= ' ')
        return false;
    switch (c) {
    case 0x00A0: case 0x2002: case 0x2003: case 0x3000: case 0x2004: case 0x2005: case 0x2006: case 0x2007:
    case 0x2008: case 0x2009: case 0x200A: case 0x200B: case 0x202F: case 0x205F: case 0xFEFF:
    case '"': case '#': case '<': case '>': case '{': case '}': case '?': case 0x7F:
        return false;
    default:
        return true;
    }
}

bool isQueryDelimiter(char32_t c)
{
    return c == '&' || c == '+' || c == '=' || c == ';';
}

bool schemeSupported(const std::u32string& text, std::size_t at, std::string_view schemes)
{
    std::size_t i = 0;
    while (i < schemes.size()) {
        while (i < schemes.size() && schemes[i] == ' ')
            ++i;
        std::size_t j = i;
        while (j < schemes.size() && schemes[j] != ' ')
            ++j;
        const std::size_t n = j - i;
        if (n && at + n <= text.size()) {
            bool same = true;
            for (std::size_t k = 0; k < n && same; ++k) {
                const char32_t a = text[at + k];
                same = a < 0x80 && std::tolower(static_cast<int>(a)) == std::tolower(static_cast<unsigned char>(schemes[i + k]));
            }
            if (same)
                return true;
        }
        i = j;
    }
    return false;
}

bool scanToUrlStart(const std::u32string& t, std::size_t start, std::string_view schemes, std::size_t& dist, std::size_t& schemeLen)
{
    std::size_t p = start, p0 = 0;
    bool inScheme = false;
    for (; p < t.size(); ++p) {
        if (!inScheme) {
            if (isSchemeStartChar(t[p]) && (p == 0 || isSchemeDelimiter(t[p - 1]))) {
                p0 = p;
                inScheme = true;
            }
        } else {
            if (t[p] == ':' && schemeSupported(t, p0, schemes)) {
                dist = p0 - start;
                schemeLen = p - p0 + 1;
                return true;
            }
            if (!isSchemeStartChar(t[p]))
                inScheme = false;
        }
    }
    schemeLen = 0;
    dist = p - start;
    return false;
}

std::size_t scanToUrlEnd(const std::u32string& t, std::size_t start)
{
    enum { HostAndPath, Query, QueryAfterDelimiter, QueryQuotes, QueryAfterQuotes, Fragment } s = HostAndPath;
    char32_t q = 0;
    std::size_t p = start;
    for (; p < t.size(); ++p) {
        const char32_t c = t[p];
        switch (s) {
        case HostAndPath:
            if (c == '?')
                s = Query;
            else if (c == '#')
                s = Fragment;
            else if (!isUrlTextChar(c))
                return p - start;
            break;
        case Query:
            if (c == '#')
                s = Fragment;
            else if (isQueryDelimiter(c))
                s = QueryAfterDelimiter;
            else if (!isUrlTextChar(c))
                return p - start;
            break;
        case QueryAfterDelimiter:
            if (c == '\'' || c == '"' || c == '`') {
                q = c;
                s = QueryQuotes;
            } else if (c == '(') {
                q = ')';
                s = QueryQuotes;
            } else if (c == '[') {
                q = ']';
                s = QueryQuotes;
            } else if (c == '{') {
                q = '}';
                s = QueryQuotes;
            } else if (isUrlTextChar(c)) {
                s = Query;
            } else {
                return p - start;
            }
            break;
        case QueryQuotes:
            if (c < ' ')
                return p - start;
            if (c == q)
                s = QueryAfterQuotes;
            break;
        case QueryAfterQuotes:
            if (isQueryDelimiter(c))
                s = QueryAfterDelimiter;
            else
                return p - start;
            break;
        case Fragment:
            if (c != '?' && !isUrlTextChar(c))
                return p - start;
            break;
        }
    }
    return p - start;
}

bool removeTrailing(const std::u32string& t, std::size_t start, std::size_t& len)
{
    if (len <= 1)
        return false;
    const char32_t last = t[start + len - 1];
    if (std::u32string_view(U".,:;?!#").find(last) != std::u32string_view::npos) {
        --len;
        return true;
    }
    const char32_t closing[] = {')', ']'}, opening[] = {'(', '['};
    for (int i = 0; i < 2; ++i)
        if (last == closing[i]) {
            int count = 0;
            for (std::size_t j = start + len - 1; j-- > start;) {
                if (t[j] == closing[i])
                    ++count;
                if (t[j] == opening[i]) {
                    if (count > 0)
                        --count;
                    else
                        return false;
                }
            }
            if (count != 0)
                return false;
            --len;
            return true;
        }
    return false;
}

void decode(std::string_view s, std::u32string& out, std::vector<std::size_t>& offsets)
{
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        char32_t cp = n == 1 ? c : c & (0x3F >> (n - 1));
        for (int k = 1; k < n && i + static_cast<std::size_t>(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        out.push_back(cp);
        offsets.push_back(i);
        i += static_cast<std::size_t>(n);
    }
    offsets.push_back(s.size());
}

} // namespace

std::vector<std::pair<std::size_t, std::size_t>> findUrls(std::string_view text, std::string_view extraSchemes)
{
    const std::string schemes = "ftp:// http:// https:// mailto: file:// " + std::string(extraSchemes);
    std::u32string t;
    std::vector<std::size_t> off;
    decode(text, t, off);
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t start = 0;
    while (start < t.size()) {
        std::size_t dist = 0, schemeLen = 0;
        if (!scanToUrlStart(t, start, schemes, dist, schemeLen))
            break;
        start += dist;
        std::size_t len = scanToUrlEnd(t, start + schemeLen);
        if (len == 0) { // a scheme alone
            start += schemeLen;
            continue;
        }
        len += schemeLen;
        // Enclosed in apostrophes or grave accents.
        if (start > 0 && (t[start - 1] == '\'' || t[start - 1] == '`') && t[start + len - 1] == t[start - 1])
            --len;
        while (removeTrailing(t, start, len)) {
        }
        if (len > schemeLen)
            out.emplace_back(off[start], off[start + len] - off[start]);
        start += std::max<std::size_t>(len, 1);
    }
    return out;
}

void markLinks(const Sci& sci, bool enable, std::string_view extraSchemes)
{
    const auto [first, last] = visibleLines(sci);
    const sptr_t from = sci.lineStart(first);
    const sptr_t to = sci.lineEnd(last);
    if (to <= from)
        return;
    sci(SCI_SETINDICATORCURRENT, kUrlIndicator);
    sci(SCI_INDICATORCLEARRANGE, static_cast<uptr_t>(from), to - from);
    if (!enable)
        return;
    sci(SCI_SETINDICATORVALUE, static_cast<uptr_t>(sci(SCI_STYLEGETFORE, STYLE_DEFAULT))); // SC_INDICFLAG_VALUEFORE
    for (const auto& [start, len] : findUrls(sci.range(from, to), extraSchemes))
        sci(SCI_INDICATORFILLRANGE, static_cast<uptr_t>(from + static_cast<sptr_t>(start)), static_cast<sptr_t>(len));
}

std::string linkAt(const Sci& sci, sptr_t pos)
{
    if (!(sci(SCI_INDICATORALLONFOR, static_cast<uptr_t>(pos)) & (1 << kUrlIndicator)))
        return {};
    const sptr_t start = sci(SCI_INDICATORSTART, kUrlIndicator, pos);
    const sptr_t end = sci(SCI_INDICATOREND, kUrlIndicator, pos);
    if (pos < start || pos > end)
        return {};
    return sci.range(start, end);
}

// ============================================================================
// Delimiter selection (upstream NppNotification.cpp, SCN_DOUBLECLICK with Ctrl)
// ============================================================================

bool selectBetweenDelimiters(const Sci& sci, sptr_t pos, char open, char close, bool multiLine)
{
    if (!open || !close)
        return false;
    sptr_t base = 0;
    std::string buf;
    if (multiLine) {
        buf = sci.text();
    } else {
        const sptr_t line = sci.lineOf(pos);
        base = sci.lineStart(line);
        buf = sci.range(base, sci.lineStart(line + 1) > base ? sci.lineStart(line + 1) : sci.length());
        if (line + 1 >= sci.lineCount())
            buf = sci.range(base, sci.length());
    }
    const auto click = static_cast<long>(pos - base);
    long left = -1, right = -1;
    if (open == close) {
        for (long i = click; i >= 0; --i) {
            if (i >= static_cast<long>(buf.size()))
                return false;
            if (buf[static_cast<std::size_t>(i)] == open && !(open == '"' && i > 0 && buf[static_cast<std::size_t>(i - 1)] == '\\')) {
                left = i;
                break;
            }
        }
        if (left == -1)
            return false;
        for (long i = click; i < static_cast<long>(buf.size()); ++i)
            if (buf[static_cast<std::size_t>(i)] == close && !(close == '"' && i > 0 && buf[static_cast<std::size_t>(i - 1)] == '\\')) {
                right = i;
                break;
            }
    } else {
        std::stack<long> lefts;
        for (long i = 0; i < static_cast<long>(buf.size()); ++i) {
            if (buf[static_cast<std::size_t>(i)] == open) {
                lefts.push(i);
            } else if (buf[static_cast<std::size_t>(i)] == close && !lefts.empty()) {
                const long l = lefts.top();
                lefts.pop();
                if (l <= click && i >= click && (left == -1 || l > left)) {
                    left = l;
                    right = i;
                }
            }
        }
    }
    if (left == -1 || right == -1)
        return false;
    sci(SCI_SETCURRENTPOS, static_cast<uptr_t>(base + right));
    sci(SCI_SETANCHOR, static_cast<uptr_t>(base + left + 1));
    return true;
}

// ============================================================================
// Auto-indent (upstream Notepad_plus::maintainIndentation)
// ============================================================================

namespace {

sptr_t lineIndent(const Sci& sci, sptr_t line)
{
    return sci(SCI_GETLINEINDENTATION, static_cast<uptr_t>(line));
}

void setLineIndent(const Sci& sci, sptr_t line, sptr_t indent)
{
    if (indent < 0)
        indent = 0;
    const sptr_t selStart = sci(SCI_GETSELECTIONSTART), selEnd = sci(SCI_GETSELECTIONEND);
    const sptr_t posBefore = sci(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line));
    sci(SCI_SETLINEINDENTATION, static_cast<uptr_t>(line), indent);
    const sptr_t posAfter = sci(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line));
    const sptr_t delta = posAfter - posBefore;
    auto adjust = [&](sptr_t p) { return p >= posBefore ? std::max(p + delta, posAfter) : (p >= posAfter ? posAfter : p); };
    if (selStart == selEnd) {
        sci(SCI_GOTOPOS, static_cast<uptr_t>(adjust(selStart)));
    } else {
        sci(SCI_SETSEL, static_cast<uptr_t>(adjust(selStart)), adjust(selEnd));
    }
}

sptr_t lineLength(const Sci& sci, sptr_t line)
{
    return sci.lineEnd(line) - sci.lineStart(line);
}

bool lineMatchesToEnd(const Sci& sci, sptr_t line, const char* expr)
{
    const sptr_t start = sci.lineStart(line), end = sci.lineEnd(line);
    const sptr_t found = searchIn(sci, expr, start, end, SCFIND_REGEXP | SCFIND_POSIX);
    return found >= 0 && sci(SCI_GETTARGETEND) == end;
}

bool isConditionExprLine(const Sci& sci, sptr_t line)
{
    if (line < 0 || line > sci.lineCount())
        return false;
    return lineMatchesToEnd(sci, line, "((else[ \t]+)?if|for|while)[ \t]*[(].*[)][ \t]*|else[ \t]*");
}

sptr_t matchedBraceBackward(const Sci& sci, sptr_t start, char target, char matched)
{
    int balance = 0;
    for (sptr_t i = start; i >= 0; --i) {
        const char c = charAt(sci, i);
        if (c == target) {
            if (balance == 0)
                return i;
            --balance;
        } else if (c == matched) {
            ++balance;
        }
    }
    return -1;
}

} // namespace

void maintainIndentation(const Sci& sci, int ch, int mode, IndentStyle style)
{
    if (mode == 0)
        return;
    const sptr_t tStart = sci(SCI_GETTARGETSTART), tEnd = sci(SCI_GETTARGETEND);
    const int eolMode = static_cast<int>(sci(SCI_GETEOLMODE));
    const sptr_t curLine = sci.lineOf(sci(SCI_GETCURRENTPOS));
    sptr_t prevLine = curLine - 1;
    const sptr_t tabWidth = sci(SCI_GETTABWIDTH);
    const bool newLine = ((eolMode == SC_EOL_CRLF || eolMode == SC_EOL_LF) && ch == '\n') || (eolMode == SC_EOL_CR && ch == '\r');
    auto restore = [&] { sci.setTarget(tStart, tEnd); };
    // Enter at the start of a line: leave it alone.
    if (newLine && prevLine >= 0 && lineLength(sci, prevLine) == 0)
        return;
    auto prevIndent = [&]() -> sptr_t {
        while (prevLine >= 0 && lineLength(sci, prevLine) == 0)
            --prevLine;
        return prevLine >= 0 ? lineIndent(sci, prevLine) : 0;
    };

    if (mode == 1 || style == IndentStyle::Plain) {
        if (newLine) {
            const sptr_t indent = prevIndent();
            if (indent > 0)
                setLineIndent(sci, curLine, indent);
        }
        return;
    }

    if (style == IndentStyle::Python) {
        if (!newLine)
            return;
        const sptr_t indent = prevIndent();
        if (prevLine < 0)
            return;
        const sptr_t start = sci.lineStart(prevLine), end = sci.lineEnd(prevLine);
        sci(SCI_COLOURISE, 0, end + 1); // the colon's style decides; lexing may lag behind typing
        const sptr_t colon = searchIn(sci, ":[ \t]*(#|$)", start, end, SCFIND_REGEXP | SCFIND_POSIX);
        if (colon >= 0 && sci(SCI_GETSTYLEINDEXAT, static_cast<uptr_t>(colon)) == SCE_P_OPERATOR)
            setLineIndent(sci, curLine, indent + tabWidth);
        else if (indent > 0)
            setLineIndent(sci, curLine, indent);
        restore();
        return;
    }

    // C-like
    if (newLine) {
        const sptr_t indent = prevIndent();
        const sptr_t cur = sci(SCI_GETCURRENTPOS);
        const char prevChar = charAt(sci, cur - (eolMode == SC_EOL_CRLF ? 3 : 2));
        const char nextChar = charAt(sci, cur);
        if (prevChar == '{') {
            if (nextChar == '}') {
                const char* eol = eolMode == SC_EOL_CRLF ? "\r\n" : eolMode == SC_EOL_LF ? "\n" : "\r";
                sci(SCI_INSERTTEXT, static_cast<uptr_t>(cur), reinterpret_cast<sptr_t>(eol));
                setLineIndent(sci, curLine + 1, indent);
            }
            setLineIndent(sci, curLine, indent + tabWidth);
        } else if (nextChar == '{') {
            setLineIndent(sci, curLine, indent);
        } else if (style == IndentStyle::CLikeNoBraceless) {
            setLineIndent(sci, curLine, indent);
        } else if (isConditionExprLine(sci, prevLine)) {
            setLineIndent(sci, curLine, indent + tabWidth);
        } else if (indent > 0) {
            if (prevLine > 0 && isConditionExprLine(sci, prevLine - 1))
                setLineIndent(sci, curLine, indent - tabWidth);
            else
                setLineIndent(sci, curLine, indent);
        }
    } else if (ch == '{') {
        const sptr_t start = sci.lineStart(curLine);
        for (sptr_t i = sci(SCI_GETCURRENTPOS) - 2; i > 0 && i >= start; --i) {
            const char c = charAt(sci, i);
            if (c != ' ' && c != '\t') {
                restore();
                return;
            }
        }
        sptr_t indent = prevIndent();
        if (prevLine >= 0 && lineMatchesToEnd(sci, prevLine, "[ \t]*\\{.*"))
            indent += tabWidth;
        setLineIndent(sci, curLine, indent);
    } else if (ch == '}') {
        sptr_t start = sci(SCI_GETCURRENTPOS);
        if (start != 0)
            start -= 1;
        const sptr_t found = matchedBraceBackward(sci, start - 1, '{', '}');
        if (found >= 0) {
            const sptr_t braceLine = sci.lineOf(found);
            if (braceLine != curLine)
                setLineIndent(sci, curLine, lineIndent(sci, braceLine));
        }
    }
    restore();
}

// ============================================================================
// Auto-insert (upstream AutoCompletion::insertMatchedChars)
// ============================================================================

std::string closeTagFor(const Sci& sci, sptr_t caretPos, bool html)
{
    if (html && sci(SCI_GETSTYLEAT, static_cast<uptr_t>(caretPos)) >= SCE_HJ_START)
        return {}; // inside a script
    const char prev = charAt(sci, caretPos - 2), prevprev = charAt(sci, caretPos - 3);
    if ((prevprev == '-' && prev == '-') || prev == '/')
        return {};
    const sptr_t tStart = sci(SCI_GETTARGETSTART), tEnd = sci(SCI_GETTARGETEND);
    const sptr_t start = searchIn(sci, "<[^\\s>]*", caretPos, 0, SCFIND_REGEXP | SCFIND_POSIX);
    const sptr_t end = sci(SCI_GETTARGETEND);
    sci.setTarget(tStart, tEnd);
    if (start < 0 || end - start < 2 || end - start > 254)
        return {};
    const std::string head = sci.range(start, end);
    if (head[1] == '/' || head[1] == '?' || head.rfind("<!--", 0) == 0)
        return {};
    if (html) {
        static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "keygen", "link",
                                            "meta", "param", "source", "track", "wbr", "!doctype"};
        for (const char* v : kVoid)
            if (strncasecmp(head.c_str() + 1, v, std::strlen(v)) == 0)
                return {};
    }
    return "</" + head.substr(1) + ">";
}

void AutoInserter::removeInvalid(const Sci& sci, char c, sptr_t pos)
{
    if (c == '\n' || c == '\r') {
        inserted_.clear();
        return;
    }
    for (int i = static_cast<int>(inserted_.size()) - 1; i >= 0; --i) {
        const Inserted& in = inserted_[static_cast<std::size_t>(i)];
        if (in.pos >= pos || sci.lineOf(pos) != sci.lineOf(in.pos))
            inserted_.erase(inserted_.begin() + i);
    }
}

sptr_t AutoInserter::search(const Sci& sci, char startChar, char endChar, sptr_t posToDetect)
{
    const sptr_t line = sci.lineOf(posToDetect);
    for (int i = static_cast<int>(inserted_.size()) - 1; i >= 0; --i) {
        const Inserted in = inserted_[static_cast<std::size_t>(i)];
        if (in.c != startChar)
            continue;
        inserted_.erase(inserted_.begin() + i);
        if (in.pos >= posToDetect || sci.lineOf(in.pos) != line)
            continue;
        const sptr_t end = sci.lineEnd(line);
        for (sptr_t j = posToDetect; j <= end; ++j) {
            const char c = charAt(sci, j);
            if (c != ' ')
                return c == endChar ? j : -1;
        }
    }
    return -1;
}

void AutoInserter::onChar(const Sci& sci, int character, const MatchedPairs& conf, int markup)
{
    const sptr_t caret = sci(SCI_GETCURRENTPOS);
    if (character > 127 || character < 0) {
        if (!inserted_.empty())
            removeInvalid(sci, '\0', caret - 1);
        return;
    }
    const char ch = static_cast<char>(character);
    const char prev = charAt(sci, caret - 2), next = charAt(sci, caret);
    const bool prevBlank = prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' || prev == '\0';
    const bool nextBlank = next == ' ' || next == '\t' || next == '\n' || next == '\r' || caret == sci.length();
    const bool nextClose = next == ')' || next == ']' || next == '}';
    const bool sandwich = (prev == '(' && next == ')') || (prev == '[' && next == ']') || (prev == '{' && next == '}');

    for (const auto& [o, c] : conf.user)
        if (o == ch && nextBlank) {
            const char s[2] = {c, '\0'};
            sci(SCI_INSERTTEXT, static_cast<uptr_t>(caret), reinterpret_cast<sptr_t>(s));
            return;
        }

    std::string insert;
    auto quote = [&](char q, bool on) {
        if (!on)
            return false;
        if (!inserted_.empty()) {
            const sptr_t pos = search(sci, q, q, caret);
            if (pos != -1) {
                sci(SCI_DELETERANGE, static_cast<uptr_t>(pos), 1);
                sci(SCI_GOTOPOS, static_cast<uptr_t>(pos));
                return true;
            }
        }
        if ((prevBlank && nextBlank) || sandwich || (prev == '(' && nextBlank) || (prevBlank && next == ')') || (prev == '[' && nextBlank) ||
            (prevBlank && next == ']') || (prev == '{' && nextBlank) || (prevBlank && next == '}')) {
            insert = std::string(1, q);
            inserted_.push_back({q, caret - 1});
        }
        return false;
    };
    switch (ch) {
    case '(':
    case '[':
    case '{': {
        const bool on = ch == '(' ? conf.parentheses : ch == '[' ? conf.brackets : conf.braces;
        if (on && (nextBlank || nextClose)) {
            insert = ch == '(' ? ")" : ch == '[' ? "]" : "}";
            removeInvalid(sci, ch, caret - 1);
            inserted_.push_back({ch, caret - 1});
        }
        break;
    }
    case '"':
        if (quote('"', conf.doubleQuotes))
            return;
        break;
    case '\'':
        if (quote('\'', conf.quotes))
            return;
        break;
    case '>':
        if (conf.htmlXmlTag && markup)
            insert = closeTagFor(sci, caret, markup == 1);
        break;
    case ')':
    case ']':
    case '}': {
        if (inserted_.empty())
            break;
        const char start = ch == ')' ? '(' : ch == ']' ? '[' : '{';
        const bool on = ch == ')' ? conf.parentheses : ch == ']' ? conf.brackets : conf.braces;
        if (!on)
            return;
        const sptr_t pos = search(sci, start, ch, caret);
        if (pos != -1) {
            sci(SCI_DELETERANGE, static_cast<uptr_t>(pos), 1);
            sci(SCI_GOTOPOS, static_cast<uptr_t>(pos));
        }
        return;
    }
    default:
        if (!inserted_.empty())
            removeInvalid(sci, ch, caret - 1);
    }
    if (!insert.empty())
        sci(SCI_INSERTTEXT, static_cast<uptr_t>(caret), reinterpret_cast<sptr_t>(insert.c_str()));
}

// ============================================================================
// Auto-completion
// ============================================================================

namespace {

constexpr int kFuncImage = 1000;
constexpr int kBoxImage = 1001;

// Upstream's 16x16 images for functions and other keywords.
const char* const kXpmFunc[] = {"16 16 36 1 ", "u c None", "  c #131313", ". c #252525", "X c #161616", "o c #202020", "O c #393939",
    "+ c #242424", "@ c #282828", "# c #4E4E4E", "$ c #343434", "% c #5B5B5B", "& c #5F5F5F", "* c #626262", "= c #404040",
    "- c #686868", "; c #434343", ": c #464646", "> c #484848", ", c #494949", "< c #515151", "1 c #929292", "2 c #9B9B9B",
    "3 c #636363", "4 c #656565", "5 c #AFAFAF", "6 c #B7B7B7", "7 c #757575", "8 c #CDCDCD", "9 c #858585", "0 c #868686",
    "q c #DDDDDD", "w c #E1E1E1", "e c #E9E9E9", "r c #EEEEEE", "t c #959595", "y c #F6F6F6",
    "uuuuuuuuuuuuuuuu", "uuuuu5o.:uuuuuuu", "uuuu8 $:.0uuuuuu", "uuuu2 uuuuuuuuuu", "uuu6$ 46uuuuuuuu", "uuuO   Ouuuuuuuu",
    "uuuu;#uuuuuuuuuu", "uuuu##u& 3uu<+uu", "uuuu#;0.@X0, >uu", "uuuu+>uuuoo >uuu", "uuuu >uuu* =uuuu", "uuuu 2uu, Xotuuu",
    "uuu# 4u< >9 %ouu", "u:,#X0uO>uu1 $ u", "u- +7uuuuuuuuuuu", "uuuuuuuuuuuuuuuu"};
const char* const kXpmBox[] = {"16 16 4 1 ", "r c None", "  c #202020", ". c #A0A0A0", "X c #E8E8E8",
    "rrrrrrrrrrrrrrrr", "rrrrrrrrrrrrrrrr", "rrrr        rrrr", "rrrr XXXXXX rrrr", "rrrr X....X rrrr", "rrrr X....X rrrr",
    "rrrr X....X rrrr", "rrrr X....X rrrr", "rrrr X....X rrrr", "rrrr X....X rrrr", "rrrr XXXXXX rrrr", "rrrr        rrrr",
    "rrrrrrrrrrrrrrrr", "rrrrrrrrrrrrrrrr", "rrrrrrrrrrrrrrrr", "rrrrrrrrrrrrrrrr"};

int compareNoCase(std::string_view a, std::string_view b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int ca = std::toupper(static_cast<unsigned char>(a[i])), cb = std::toupper(static_cast<unsigned char>(b[i]));
        if (ca != cb)
            return ca - cb;
    }
    return static_cast<int>(a.size()) - static_cast<int>(b.size());
}

void sortWords(std::vector<std::string>& v, bool ignoreCase)
{
    if (ignoreCase)
        std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return compareNoCase(a, b) < 0; });
    else
        std::sort(v.begin(), v.end());
}

bool isWordChar(char c, const std::string& extra)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || extra.find(c) != std::string::npos;
}

} // namespace

bool ApiFile::load(const fs::path& file)
{
    pugi::xml_document doc;
    if (!doc.load_file(file.c_str()))
        return false;
    const pugi::xml_node ac = doc.child("NotepadPlus").child("AutoComplete");
    if (!ac)
        return false;
    if (const pugi::xml_node env = ac.child("Environment")) {
        ignoreCase = std::string_view(env.attribute("ignoreCase").as_string("yes")) != "no";
        auto first = [&](const char* a, char& out) {
            if (const char* v = env.attribute(a).as_string(); v && *v)
                out = v[0];
        };
        first("startFunc", start);
        first("stopFunc", stop);
        first("paramSeparator", param);
        first("terminal", terminal);
        additionalWordChars = env.attribute("additionalWordChar").as_string();
    }
    for (const pugi::xml_node k : ac.children("KeyWord")) {
        ApiKeyword kw;
        kw.name = k.attribute("name").as_string();
        if (kw.name.empty())
            continue;
        kw.func = std::string_view(k.attribute("func").as_string()) == "yes";
        for (const pugi::xml_node o : k.children("Overload")) {
            if (!o.attribute("retVal"))
                continue;
            ApiOverload ov{o.attribute("retVal").as_string(), o.attribute("descr").as_string(), {}};
            for (const pugi::xml_node p : o.children("Param"))
                if (p.attribute("name"))
                    ov.params.emplace_back(p.attribute("name").as_string());
            kw.overloads.push_back(std::move(ov));
        }
        maxLen = std::max(maxLen, kw.name.size());
        keywords.push_back(std::move(kw));
    }
    const bool ic = ignoreCase;
    std::stable_sort(keywords.begin(), keywords.end(), [ic](const ApiKeyword& a, const ApiKeyword& b) {
        return ic ? compareNoCase(a.name, b.name) < 0 : a.name < b.name;
    });
    return true;
}

const ApiKeyword* ApiFile::find(std::string_view name) const
{
    for (const ApiKeyword& k : keywords)
        if (ignoreCase ? compareNoCase(k.name, name) == 0 : k.name == name)
            return &k;
    return nullptr;
}

const ApiFile* ApiCatalog::forLanguage(const std::string& name)
{
    if (name.empty())
        return nullptr;
    auto it = cache_.find(name);
    if (it != cache_.end())
        return it->second.get();
    std::unique_ptr<ApiFile> api;
    for (const std::string& candidate : {name, fs::path(name).stem().string()}) {
        auto a = std::make_unique<ApiFile>();
        if (a->load(dir_ / (candidate + ".xml"))) {
            api = std::move(a);
            break;
        }
    }
    return (cache_[name] = std::move(api)).get();
}

bool AutoCompleter::showList(const Sci& sci, int type, bool autoInsert, const ApiFile* api, bool ignoreNumbers)
{
    // type: 0 function list, 1 words, 2 functions and words, 3 brief function list
    if ((type == 0 || type == 3) && !api)
        return false;
    const sptr_t cur = sci(SCI_GETCURRENTPOS);
    const sptr_t start = sci(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(cur), true);
    if (cur == start)
        return false;
    const std::string begin = sci.range(start, cur);
    if (begin.size() >= 256 || (type == 0 && api && begin.size() >= api->maxLen + 1))
        return false;
    const bool ignoreCase = api ? api->ignoreCase : false;
    const std::string boxId = "\x1E" + std::to_string(kBoxImage), funcId = "\x1E" + std::to_string(kFuncImage);
    std::vector<std::string> words;
    auto add = [&words](const std::string& w) {
        if (std::find(words.begin(), words.end(), w) == words.end())
            words.push_back(w);
    };
    if (type == 0) {
        for (const ApiKeyword& k : api->keywords)
            add(k.name + (k.func ? funcId : boxId));
    } else {
        if (type == 1 || type == 2) {
            const sptr_t end = sci(SCI_WORDENDPOSITION, static_cast<uptr_t>(cur), true);
            const std::string exclude = sci.range(start, end);
            const bool digits = std::all_of(begin.begin(), begin.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            if (!(ignoreNumbers && digits)) {
                const std::string expr = "\\<" + regexEscape(begin) + "[^ \\t\\n\\r.,;:\"(){}=<>'+!?\\[\\]]+";
                int flags = SCFIND_WORDSTART | SCFIND_REGEXP | SCFIND_POSIX;
                if (!ignoreCase || !api)
                    flags |= SCFIND_MATCHCASE;
                const sptr_t tStart = sci(SCI_GETTARGETSTART), tEnd = sci(SCI_GETTARGETEND);
                const sptr_t len = sci.length();
                sptr_t pos = searchIn(sci, expr, 0, len, flags);
                while (pos >= 0) {
                    const sptr_t ws = sci(SCI_GETTARGETSTART), we = sci(SCI_GETTARGETEND);
                    if (we - ws < 256) {
                        const std::string w = sci.range(ws, we);
                        if (w != exclude) {
                            const ApiKeyword* k = api ? api->find(w) : nullptr;
                            add(k && k->name == w ? w + (k->func ? funcId : boxId) : w);
                        }
                    }
                    if (we <= ws)
                        break;
                    pos = searchIn(sci, expr, we, len, flags);
                }
                sci.setTarget(tStart, tEnd);
            }
        }
        if ((type == 2 || type == 3) && api)
            for (const ApiKeyword& k : api->keywords) {
                const bool match = ignoreCase ? compareNoCase(std::string_view(k.name).substr(0, begin.size()), begin) == 0
                                              : k.name.compare(0, begin.size(), begin) == 0;
                if (match)
                    add(k.name + (k.func ? funcId : boxId));
            }
        if (words.empty())
            return false;
        if (type == 1 && words.size() == 1 && autoInsert) {
            std::string w = words.front();
            if (const std::size_t sep = w.find('\x1E'); sep != std::string::npos)
                w.resize(sep);
            sci.setTarget(start, cur);
            const sptr_t n = sci.replaceTarget(w);
            sci(SCI_GOTOPOS, static_cast<uptr_t>(start + n));
            return true;
        }
        sortWords(words, ignoreCase);
    }
    if (!imagesRegistered_) {
        sci(SCI_REGISTERIMAGE, kFuncImage, reinterpret_cast<sptr_t>(kXpmFunc));
        sci(SCI_REGISTERIMAGE, kBoxImage, reinterpret_cast<sptr_t>(kXpmBox));
        imagesRegistered_ = true;
    }
    std::string list;
    for (const std::string& w : words)
        list += (list.empty() ? "" : " ") + w;
    sci(SCI_AUTOCSETTYPESEPARATOR, '\x1E');
    sci(SCI_AUTOCSETSEPARATOR, ' ');
    sci(SCI_AUTOCSETIGNORECASE, ignoreCase);
    sci(SCI_AUTOCSETCASEINSENSITIVEBEHAVIOUR, ignoreCase);
    sci(SCI_AUTOCSHOW, static_cast<uptr_t>(cur - start), reinterpret_cast<sptr_t>(list.c_str()));
    return true;
}

bool AutoCompleter::showFunctionList(const Sci& sci, const ApiFile* api)
{
    return showList(sci, 0, false, api, false);
}

bool AutoCompleter::showWordList(const Sci& sci, const ApiFile* api, bool autoInsert, bool ignoreNumbers)
{
    return showList(sci, 1, autoInsert, api, ignoreNumbers);
}

bool AutoCompleter::showCallTip(const Sci& sci, const ApiFile* api)
{
    return api && updateCallTip(sci, 0, true, api);
}

void AutoCompleter::closeCallTip(const Sci& sci)
{
    if (!tipShown_)
        return;
    sci(SCI_CALLTIPCANCEL);
    tipShown_ = false;
    overload_ = 0;
}

void AutoCompleter::callTipClick(const Sci& sci, int direction)
{
    if (!tipShown_ || !tipFunc_ || tipFunc_->overloads.empty())
        return;
    const std::size_t n = tipFunc_->overloads.size();
    overload_ = direction == 1 ? (overload_ + n - 1) % n : (overload_ + 1) % n;
    drawCallTip(sci);
}

// Upstream FunctionCallTip::getCursorFunction: the function whose argument
// list the caret is in, and which argument.
bool AutoCompleter::findFunction(const Sci& sci, const ApiFile& api)
{
    const sptr_t cur = sci(SCI_GETCURRENTPOS);
    const sptr_t line = sci.lineOf(cur);
    const sptr_t lineStart = sci.lineStart(line);
    const sptr_t offset = cur - lineStart;
    if (offset < 2 || sci.lineEnd(line) - lineStart + 3 >= 256)
        return false;
    const std::string text = sci.range(lineStart, cur);
    struct Token {
        std::string s;
        bool id;
    };
    std::vector<Token> tokens;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (isWordChar(c, api.additionalWordChars)) {
            std::size_t j = i;
            while (j < text.size() && isWordChar(text[j], api.additionalWordChars))
                ++j;
            tokens.push_back({text.substr(i, j - i), true});
            i = j - 1;
        } else if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < text.size() && text[j] != c) {
                if (text[j] == '\\')
                    ++j;
                ++j;
            }
            i = j;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            tokens.push_back({std::string(1, c), false});
        }
    }
    struct Values {
        int lastId = -1, lastFunc = -1;
        std::size_t param = 0;
    };
    std::vector<Values> stack;
    Values v;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.id) {
            v.lastId = static_cast<int>(i);
            continue;
        }
        const char c = t.s[0];
        if (c == api.start) {
            stack.push_back(v);
            if (i > 0 && v.lastId == static_cast<int>(i) - 1) {
                v.lastFunc = v.lastId;
                v.param = 0;
            } else {
                v.lastFunc = -1;
            }
        } else if (c == api.param && v.lastFunc > -1) {
            ++v.param;
        } else if (c == api.stop) {
            if (!stack.empty()) {
                v = stack.back();
                stack.pop_back();
            } else {
                v = Values{};
            }
        } else if (c == api.terminal) {
            stack.clear();
            v = Values{};
        }
    }
    while (v.lastFunc == -1 && !stack.empty()) {
        v = stack.back();
        stack.pop_back();
    }
    if (v.lastFunc < 0)
        return false;
    const ApiKeyword* k = api.find(tokens[static_cast<std::size_t>(v.lastFunc)].s);
    if (!k || !k->func || k->overloads.empty())
        return false;
    if (k != tipFunc_)
        overload_ = 0;
    tipFunc_ = k;
    param_ = v.param;
    return true;
}

void AutoCompleter::drawCallTip(const Sci& sci)
{
    const ApiFile& api = *tipApi_;
    const auto& overloads = tipFunc_->overloads;
    if (overload_ >= overloads.size())
        overload_ = 0;
    if (param_ >= overloads[overload_].params.size() + 1)
        for (std::size_t i = 0; i < overloads.size(); ++i)
            if (param_ < overloads[i].params.size() + 1) {
                overload_ = i;
                break;
            }
    const ApiOverload& o = overloads[overload_];
    std::ostringstream s;
    if (overloads.size() > 1)
        s << "\001" << overload_ + 1 << " of " << overloads.size() << "\002";
    s << o.retVal << ' ' << tipFunc_->name << ' ' << api.start;
    std::size_t hlStart = 0, hlEnd = 0;
    for (std::size_t i = 0; i < o.params.size(); ++i) {
        if (i == param_) {
            hlStart = s.str().size();
            hlEnd = hlStart + o.params[i].size();
        }
        s << o.params[i];
        if (i + 1 < o.params.size())
            s << api.param << ' ';
    }
    s << api.stop;
    if (!o.descr.empty())
        s << "\n" << o.descr;
    if (tipShown_)
        sci(SCI_CALLTIPCANCEL);
    else
        tipPos_ = sci(SCI_GETCURRENTPOS);
    const std::string text = s.str();
    sci(SCI_CALLTIPSHOW, static_cast<uptr_t>(tipPos_), reinterpret_cast<sptr_t>(text.c_str()));
    tipShown_ = true;
    if (hlStart != hlEnd)
        sci(SCI_CALLTIPSETHLT, hlStart, static_cast<sptr_t>(hlEnd));
}

bool AutoCompleter::updateCallTip(const Sci& sci, int ch, bool needShown, const ApiFile* api)
{
    if (!api)
        return false;
    const bool visible = tipShown_ && sci(SCI_CALLTIPACTIVE);
    if (!visible)
        tipShown_ = false;
    if (!needShown && ch != api->start && ch != api->param && !visible)
        return false;
    tipApi_ = api;
    if (!findFunction(sci, *api)) {
        closeCallTip(sci);
        return false;
    }
    drawCallTip(sci);
    return true;
}

void AutoCompleter::onChar(const Sci& sci, int ch, const AutoCompleteOptions& o, const ApiFile* api)
{
    if (!ch)
        return;
    if (o.paramHint || (tipShown_ && sci(SCI_CALLTIPACTIVE)))
        if (updateCallTip(sci, ch, false, api))
            return;
    if (!o.enable)
        return;
    if (!api && o.mode == 0)
        return;
    if (!o.brief && sci(SCI_AUTOCACTIVE))
        return;
    const sptr_t cur = sci(SCI_GETCURRENTPOS);
    const sptr_t start = sci(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(cur), true);
    if (cur - start < std::max(o.fromChar, 1) || cur - start >= 64)
        return;
    if (o.mode == 1)
        showList(sci, 1, false, api, o.ignoreNumbers);
    else if (o.mode == 0)
        showList(sci, o.brief ? 3 : 0, false, api, o.ignoreNumbers);
    else
        showList(sci, 2, false, api, o.ignoreNumbers);
}

} // namespace npp::core
