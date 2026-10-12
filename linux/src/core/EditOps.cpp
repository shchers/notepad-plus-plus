#include "core/EditOps.h"

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <random>
#include <set>
#include <unordered_set>

namespace npp::core {

namespace {

// ---- UTF-8 <-> code points --------------------------------------------------

std::u32string toU32(std::string_view s)
{
    std::u32string out;
    out.reserve(s.size());
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end) {
        gunichar c = g_utf8_get_char_validated(p, end - p);
        if (c == static_cast<gunichar>(-1) || c == static_cast<gunichar>(-2)) {
            out.push_back(static_cast<unsigned char>(*p)); // keep invalid bytes as Latin-1
            ++p;
            continue;
        }
        out.push_back(c);
        p = g_utf8_next_char(p);
    }
    return out;
}

std::string toUtf8(std::u32string_view s)
{
    std::string out;
    out.reserve(s.size());
    char buf[6];
    for (char32_t c : s)
        out.append(buf, static_cast<std::size_t>(g_unichar_to_utf8(c, buf)));
    return out;
}

bool isAlpha(char32_t c) { return g_unichar_isalpha(c); }
bool isAlnum(char32_t c) { return g_unichar_isalnum(c); }
char32_t upper(char32_t c) { return g_unichar_toupper(c); }
char32_t lower(char32_t c) { return g_unichar_tolower(c); }
bool isSingleQuote(char32_t c) { return c == U'\'' || c == U'’' || c == U'‘'; }

// ---- line helpers -----------------------------------------------------------

std::string eolString(const Sci& sci)
{
    switch (sci(SCI_GETEOLMODE)) {
    case SC_EOL_CRLF: return "\r\n";
    case SC_EOL_CR: return "\r";
    default: return "\n";
    }
}

bool isRectangular(const Sci& sci)
{
    const sptr_t mode = sci(SCI_GETSELECTIONMODE);
    return mode == SC_SEL_RECTANGLE || mode == SC_SEL_THIN;
}

// Upstream getSelectionLinesRange: a selection ending at column 0 does not
// include that line.
std::pair<sptr_t, sptr_t> selectionLines(const Sci& sci)
{
    const sptr_t s = sci(SCI_GETSELECTIONSTART), e = sci(SCI_GETSELECTIONEND);
    const sptr_t l1 = sci.lineOf(s);
    sptr_t l2 = sci.lineOf(e);
    if (l1 != l2 && sci.lineStart(l2) == e)
        --l2;
    return {l1, l2};
}

bool hasSelection(const Sci& sci) { return sci(SCI_GETANCHOR) != sci(SCI_GETCURRENTPOS); }

// Range covering lines [from, to] including the EOL of `to`.
std::pair<sptr_t, sptr_t> linesRange(const Sci& sci, sptr_t from, sptr_t to)
{
    return {sci.lineStart(from), sci.lineStart(to) + sci(SCI_LINELENGTH, static_cast<uptr_t>(to))};
}

std::vector<std::string> split(const std::string& text, const std::string& eol)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t n = text.find(eol, pos);
        if (n == std::string::npos) {
            out.push_back(text.substr(pos));
            return out;
        }
        out.push_back(text.substr(pos, n - pos));
        pos = n + eol.size();
    }
}

std::string join(const std::vector<std::string>& lines, const std::string& eol)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += eol;
        out += lines[i];
    }
    return out;
}

// Upstream sortLines / removeAnyDuplicateLines: works on lines [from, to] as
// strings; the last line of a partial range keeps its EOL.
template <class Fn>
bool transformLines(const Sci& sci, sptr_t from, sptr_t to, Fn&& fn)
{
    if (from >= to)
        return false;
    const auto [start, end] = linesRange(sci, from, to);
    const std::string text = sci.range(start, end);
    const std::string eol = eolString(sci);
    std::vector<std::string> lines = split(text, eol);
    const bool wholeDoc = to == sci.lineCount() - 1;
    if (!wholeDoc && !lines.empty() && lines.back().empty())
        lines.pop_back();
    if (!fn(lines))
        return false;
    std::string joined = join(lines, eol);
    if (!wholeDoc)
        joined += eol;
    if (joined != text) {
        sci.setTarget(start, end);
        sci.replaceTarget(joined);
    }
    return true;
}

// Selection scope used by trim / tab conversion: the whole document without a
// selection, else the selected lines (expanded to whole lines).
std::pair<sptr_t, sptr_t> blankOpLines(const Sci& sci)
{
    if (!hasSelection(sci))
        return {0, sci.lineCount() - 1};
    return selectionLines(sci);
}

// Replaces line `l` (without its EOL) when `fn` changes it. Returns the size delta.
template <class Fn>
void rewriteLines(const Sci& sci, sptr_t from, sptr_t to, Fn&& fn)
{
    sci(SCI_BEGINUNDOACTION);
    for (sptr_t l = from; l <= to; ++l) {
        const std::string before = sci.lineText(l);
        const std::string after = fn(before);
        if (after != before) {
            sci.setTarget(sci.lineStart(l), sci.lineEnd(l));
            sci.replaceTarget(after);
        }
    }
    sci(SCI_ENDUNDOACTION);
}

// ---- numbers for the numeric sorts (upstream Sorters.h) ---------------------

std::u32string sortKey(const std::u32string& s, std::size_t fromCol, std::size_t toCol)
{
    if (toCol == 0)
        return s;
    if (s.size() < fromCol)
        return {};
    if (fromCol == toCol)
        return s.substr(fromCol);
    return s.substr(fromCol, toCol - fromCol);
}

// Natural comparison of IntegerSorter: digit runs (with an optional leading
// '-') compare as numbers of any length, other runs as text.
int compareNatural(const std::u32string& a, const std::u32string& b)
{
    auto digit = [](char32_t c) { return c >= U'0' && c <= U'9'; };
    std::size_t i = 0, j = 0;
    long long r = 0;
    while (r == 0) {
        if (i >= a.size() || j >= b.size())
            return a.compare(std::min(i, a.size()), std::u32string::npos, b, std::min(j, b.size()), std::u32string::npos);
        bool an = digit(a[i]), bn = digit(b[j]);
        int as = 1, bs = 1;
        if (!an && i + 1 < a.size()) {
            an = a[i] == U'-' && digit(a[i + 1]);
            as = -1;
        }
        if (!bn && j + 1 < b.size()) {
            bn = b[j] == U'-' && digit(b[j + 1]);
            bs = -1;
        }
        if (an != bn) {
            r = static_cast<long long>(a[i]) - static_cast<long long>(b[j]);
            ++i;
            ++j;
        } else if (an) {
            if (as != bs) {
                r = as == 1 ? 1 : -1;
            } else {
                if (as == -1) {
                    ++i;
                    ++j;
                }
                std::size_t ae = i, be = j;
                while (ae < a.size() && digit(a[ae]))
                    ++ae;
                while (be < b.size() && digit(b[be]))
                    ++be;
                int az = 0, bz = 0;
                while (i < a.size() && a[i] == U'0') {
                    ++az;
                    ++i;
                }
                while (j < b.size() && b[j] == U'0') {
                    ++bz;
                    ++j;
                }
                const std::size_t al = ae - i, bl = be - j;
                if (al > bl) {
                    r = as;
                } else if (al < bl) {
                    r = -as;
                } else {
                    while (r == 0 && i < a.size() && digit(a[i]) && j < b.size() && digit(b[j])) {
                        r = (static_cast<long long>(a[i]) - static_cast<long long>(b[j])) * as;
                        ++i;
                        ++j;
                    }
                    if (r == 0)
                        r = bz - az;
                }
            }
        } else {
            if (a[i] == U'-')
                ++i;
            if (b[j] == U'-')
                ++j;
            // npos past the end: the next round compares what is left.
            const std::size_t ae = a.find_first_of(U"1234567890-", i);
            const std::size_t be = b.find_first_of(U"1234567890-", j);
            r = a.compare(i, ae == std::u32string::npos ? ae : ae - i, b, j, be == std::u32string::npos ? be : be - j);
            i = ae;
            j = be;
        }
    }
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

// stod in the "C" locale on the admissible prefix; throws on failure as upstream.
bool parseDecimal(const std::u32string& key, char32_t decimalSep, double& out, bool& empty)
{
    std::string s;
    const std::u32string allowed = std::u32string(U" \t\r\n0123456789-") + decimalSep;
    for (char32_t c : key) {
        if (allowed.find(c) == std::u32string::npos)
            break;
        s += c == decimalSep ? '.' : static_cast<char>(c);
    }
    empty = s.find_first_not_of(" \t\r\n") == std::string::npos;
    if (empty)
        return true;
    errno = 0;
    char* end = nullptr;
    out = g_ascii_strtod(s.c_str(), &end); // locale independent, like upstream's en-US locale
    return end != s.c_str() && errno != ERANGE;
}

// Text changed by a case conversion keeps its byte length only for ASCII, so
// the caller adjusts selections by the returned delta.
sptr_t convertRange(const Sci& sci, sptr_t from, sptr_t to, TextCase c)
{
    if (to <= from)
        return 0;
    const std::string before = sci.range(from, to);
    const std::string after = convertCase(before, c);
    if (after == before)
        return 0;
    sci.setTarget(from, to);
    sci.replaceTarget(after);
    return static_cast<sptr_t>(after.size()) - static_cast<sptr_t>(before.size());
}

} // namespace

// ============================================================================
// Case
// ============================================================================

std::string convertCase(std::string_view utf8, TextCase tc)
{
    std::u32string s = toU32(utf8);
    const std::size_t n = s.size();
    switch (tc) {
    case TextCase::Upper:
        for (char32_t& c : s)
            c = upper(c);
        break;
    case TextCase::Lower:
        for (char32_t& c : s)
            c = lower(c);
        break;
    case TextCase::ProperForce:
    case TextCase::ProperBlend:
        for (std::size_t i = 0; i < n; ++i) {
            if (!isAlpha(s[i]))
                continue;
            if (i >= 2 && isSingleQuote(s[i - 1]) && isAlnum(s[i - 2])) {
                if (tc == TextCase::ProperForce)
                    s[i] = lower(s[i]); // "don't", "it's"
            } else if (i < 1 || !isAlnum(s[i - 1])) {
                s[i] = upper(s[i]);
            } else if (tc == TextCase::ProperForce) {
                s[i] = lower(s[i]);
            }
        }
        break;
    case TextCase::SentenceForce:
    case TextCase::SentenceBlend: {
        bool newSentence = true, wasR = false, wasN = false;
        for (std::size_t i = 0; i < n; ++i) {
            const char32_t c = s[i];
            if (isAlpha(c)) {
                if (newSentence) {
                    s[i] = upper(c);
                    newSentence = false;
                } else if (tc == TextCase::SentenceForce) {
                    s[i] = lower(c);
                }
                wasR = wasN = false;
                // "i" on its own is a word.
                if (s[i] == U'i' && i >= 1 && (g_unichar_isspace(s[i - 1]) || s[i - 1] == U'(' || s[i - 1] == U'"')
                    && i + 1 < n && (g_unichar_isspace(s[i + 1]) || s[i + 1] == U'\''))
                    s[i] = U'I';
            } else if (c == U'.' || c == U'!' || c == U'?') {
                newSentence = !(i + 1 == n || isAlnum(s[i + 1]));
            } else if (c == U'\r') {
                if (wasR)
                    newSentence = true;
                else
                    wasR = true;
            } else if (c == U'\n') {
                if (wasN)
                    newSentence = true;
                else
                    wasN = true;
            }
        }
        break;
    }
    case TextCase::Invert:
        for (char32_t& c : s)
            c = g_unichar_islower(c) ? upper(c) : lower(c);
        break;
    case TextCase::Random: {
        static std::mt19937 rng(static_cast<unsigned>(std::time(nullptr)));
        for (char32_t& c : s)
            if (isAlpha(c))
                c = (rng() & 1) ? upper(c) : lower(c);
        break;
    }
    }
    return toUtf8(s);
}

void convertSelectionCase(const Sci& sci, TextCase c)
{
    const sptr_t n = sci(SCI_GETSELECTIONS);
    if (n > 1) {
        struct Sel {
            sptr_t anchor, caret;
        };
        std::vector<std::pair<sptr_t, Sel>> sels; // sorted by start
        for (sptr_t i = 0; i < n; ++i)
            sels.push_back({std::min(sci(SCI_GETSELECTIONNANCHOR, static_cast<uptr_t>(i)),
                                     sci(SCI_GETSELECTIONNCARET, static_cast<uptr_t>(i))),
                            {sci(SCI_GETSELECTIONNANCHOR, static_cast<uptr_t>(i)), sci(SCI_GETSELECTIONNCARET, static_cast<uptr_t>(i))}});
        const sptr_t main = sci(SCI_GETMAINSELECTION);
        std::vector<sptr_t> order(sels.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<sptr_t>(i);
        std::sort(order.begin(), order.end(), [&](sptr_t a, sptr_t b) { return sels[a].first < sels[b].first; });

        sci(SCI_BEGINUNDOACTION);
        sptr_t delta = 0;
        for (sptr_t idx : order) {
            Sel& s = sels[idx].second;
            const bool forward = s.anchor <= s.caret;
            sptr_t lo = std::min(s.anchor, s.caret) + delta, hi = std::max(s.anchor, s.caret) + delta;
            const sptr_t d = convertRange(sci, lo, hi, c);
            delta += d;
            hi += d;
            s = forward ? Sel{lo, hi} : Sel{hi, lo};
        }
        sci(SCI_ENDUNDOACTION);
        // Restore the selections in their original order.
        sci(SCI_SETSELECTION, static_cast<uptr_t>(sels[0].second.caret), sels[0].second.anchor);
        for (std::size_t i = 1; i < sels.size(); ++i)
            sci(SCI_ADDSELECTION, static_cast<uptr_t>(sels[i].second.caret), sels[i].second.anchor);
        sci(SCI_SETMAINSELECTION, static_cast<uptr_t>(main));
        return;
    }
    const sptr_t s = sci(SCI_GETSELECTIONSTART), e = sci(SCI_GETSELECTIONEND);
    if (s < e) {
        const sptr_t d = convertRange(sci, s, e, c);
        sci(SCI_SETSEL, static_cast<uptr_t>(s), e + d);
    }
}

// ============================================================================
// Sorting
// ============================================================================

bool sortStrings(std::vector<std::string>& lines, SortKind kind, bool descending, std::size_t fromCol, std::size_t toCol,
                 std::size_t* badLine)
{
    auto keyed = [&](auto&& cmp) {
        // cmp(a, b) on the sort keys as code points; stable when sorting columns, as upstream.
        std::vector<std::pair<std::u32string, std::string>> v;
        v.reserve(lines.size());
        for (std::string& l : lines)
            v.emplace_back(sortKey(toU32(l), fromCol, toCol), std::move(l));
        auto less = [&](const auto& a, const auto& b) {
            const int r = cmp(a.first, b.first);
            return descending ? r > 0 : r < 0;
        };
        std::stable_sort(v.begin(), v.end(), less);
        for (std::size_t i = 0; i < v.size(); ++i)
            lines[i] = std::move(v[i].second);
    };

    switch (kind) {
    case SortKind::Lexicographic:
        keyed([](const std::u32string& a, const std::u32string& b) { return a.compare(b); });
        return true;
    case SortKind::LexicographicIgnoreCase:
        keyed([](const std::u32string& a, const std::u32string& b) {
            const std::size_t n = std::min(a.size(), b.size());
            for (std::size_t i = 0; i < n; ++i) {
                const char32_t x = lower(a[i]), y = lower(b[i]);
                if (x != y)
                    return x < y ? -1 : 1;
            }
            return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
        });
        return true;
    case SortKind::Locale:
        // Upstream SortLocale: the user's collation (CompareStringEx).
        keyed([](const std::u32string& a, const std::u32string& b) {
            const std::string x = toUtf8(a), y = toUtf8(b);
            const int r = g_utf8_collate(x.c_str(), y.c_str());
            return r < 0 ? -1 : r > 0 ? 1 : 0;
        });
        return true;
    case SortKind::Integer:
        keyed(compareNatural);
        return true;
    case SortKind::Length:
        keyed([](const std::u32string& a, const std::u32string& b) {
            return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
        });
        return true;
    case SortKind::Reverse:
        std::reverse(lines.begin(), lines.end());
        return true;
    case SortKind::Random: {
        std::shuffle(lines.begin(), lines.end(), std::default_random_engine(static_cast<unsigned>(std::time(nullptr))));
        return true;
    }
    case SortKind::DecimalComma:
    case SortKind::DecimalDot: {
        // Upstream NumericSorter: blank lines go first (last when descending).
        const char32_t sep = kind == SortKind::DecimalComma ? U',' : U'.';
        std::vector<std::pair<double, std::size_t>> nums;
        std::vector<std::size_t> empties;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            double d = 0;
            bool empty = false;
            if (!parseDecimal(sortKey(toU32(lines[i]), fromCol, toCol), sep, d, empty)) {
                if (badLine)
                    *badLine = i;
                return false;
            }
            if (empty)
                empties.push_back(i);
            else
                nums.emplace_back(d, i);
        }
        std::stable_sort(nums.begin(), nums.end(), [descending](const auto& a, const auto& b) {
            return descending ? a.first > b.first : a.first < b.first;
        });
        std::vector<std::string> out;
        out.reserve(lines.size());
        if (!descending)
            for (std::size_t i : empties)
                out.push_back(std::move(lines[i]));
        for (const auto& [d, i] : nums)
            out.push_back(std::move(lines[i]));
        if (descending)
            for (std::size_t i : empties)
                out.push_back(std::move(lines[i]));
        lines = std::move(out);
        return true;
    }
    }
    return false;
}

bool sortLines(const Sci& sci, SortKind kind, bool descending, std::string* error)
{
    sptr_t from = 0, to = 0;
    std::size_t fromCol = 0, toCol = 0;
    if (sci(SCI_GETSELECTIONS) > 1) {
        if (!sci(SCI_SELECTIONISRECTANGLE) && sci(SCI_GETSELECTIONMODE) != SC_SEL_THIN)
            return false;
        const sptr_t anchor = sci(SCI_GETRECTANGULARSELECTIONANCHOR), caret = sci(SCI_GETRECTANGULARSELECTIONCARET);
        const sptr_t al = sci.lineOf(anchor), cl = sci.lineOf(caret);
        from = std::min(al, cl);
        to = std::max(al, cl);
        // Columns in characters, virtual space included.
        auto col = [&](sptr_t pos, sptr_t line, sptr_t virt) {
            return static_cast<std::size_t>(sci(SCI_COUNTCHARACTERS, static_cast<uptr_t>(sci.lineStart(line)), pos) + virt);
        };
        const std::size_t ac = col(anchor, al, sci(SCI_GETRECTANGULARSELECTIONANCHORVIRTUALSPACE));
        const std::size_t cc = col(caret, cl, sci(SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE));
        fromCol = std::min(ac, cc);
        toCol = std::max(ac, cc);
    } else if (sci(SCI_GETSELECTIONSTART) != sci(SCI_GETSELECTIONEND)) {
        std::tie(from, to) = selectionLines(sci);
        if (from == to)
            return false; // a single line is not sorted
    } else {
        to = sci.lineCount() - 1;
    }

    std::size_t bad = 0;
    bool failed = false;
    sci(SCI_BEGINUNDOACTION);
    const bool ok = transformLines(sci, from, to, [&](std::vector<std::string>& lines) {
        if (sortStrings(lines, kind, descending, fromCol, toCol, &bad))
            return true;
        failed = true;
        return false;
    });
    sci(SCI_ENDUNDOACTION);
    if (failed && error)
        *error = std::to_string(static_cast<long long>(from) + static_cast<long long>(bad) + 1);
    return ok;
}

// ============================================================================
// Blank operations
// ============================================================================

void trim(const Sci& sci, Trim which)
{
    if (isRectangular(sci) && sci(SCI_GETSELECTIONS) > 1)
        return;
    const auto [from, to] = blankOpLines(sci);
    const sptr_t anchor = sci(SCI_GETANCHOR), caret = sci(SCI_GETCURRENTPOS);
    const bool whole = !hasSelection(sci);
    const sptr_t len = sci.length();
    rewriteLines(sci, from, to, [which](const std::string& l) {
        std::size_t b = 0, e = l.size();
        if (which != Trim::Trailing)
            while (b < e && (l[b] == ' ' || l[b] == '\t'))
                ++b;
        if (which != Trim::Leading)
            while (e > b && (l[e - 1] == ' ' || l[e - 1] == '\t'))
                --e;
        return l.substr(b, e - b);
    });
    if (!whole) {
        if (sci.length() == len) {
            sci(SCI_SETANCHOR, static_cast<uptr_t>(anchor));
            sci(SCI_SETCURRENTPOS, static_cast<uptr_t>(caret));
        } else {
            sci(SCI_SETSEL, static_cast<uptr_t>(sci.lineStart(from)), sci.lineEnd(to));
        }
    }
}

void eolToSpace(const Sci& sci)
{
    if (isRectangular(sci) && sci(SCI_GETSELECTIONS) > 1)
        return;
    sci(hasSelection(sci) ? SCI_TARGETFROMSELECTION : SCI_TARGETWHOLEDOCUMENT);
    sci(SCI_LINESJOIN);
}

void trimAll(const Sci& sci)
{
    sci(SCI_BEGINUNDOACTION);
    const bool whole = !hasSelection(sci);
    trim(sci, Trim::Both);
    if (whole || hasSelection(sci))
        eolToSpace(sci);
    sci(SCI_ENDUNDOACTION);
}

void tabsToSpaces(const Sci& sci)
{
    if (isRectangular(sci) && sci(SCI_GETSELECTIONS) > 1)
        return;
    const std::size_t tab = static_cast<std::size_t>(std::max<sptr_t>(1, sci(SCI_GETTABWIDTH)));
    const auto [from, to] = blankOpLines(sci);
    rewriteLines(sci, from, to, [tab](const std::string& l) {
        if (l.find('\t') == std::string::npos)
            return l;
        std::string out;
        std::size_t col = 0;
        for (char c : l) {
            if (c == '\t') {
                const std::size_t n = tab - col % tab;
                out.append(n, ' ');
                col += n;
            } else {
                out += c;
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) // count characters, not UTF-8 continuation bytes
                    ++col;
            }
        }
        return out;
    });
}

void spacesToTabs(const Sci& sci, bool leadingOnly)
{
    if (isRectangular(sci) && sci(SCI_GETSELECTIONS) > 1)
        return;
    const std::size_t tab = static_cast<std::size_t>(std::max<sptr_t>(1, sci(SCI_GETTABWIDTH)));
    const auto [from, to] = blankOpLines(sci);
    rewriteLines(sci, from, to, [tab, leadingOnly](const std::string& l) {
        std::string out;
        std::size_t col = 0, i = 0;
        bool leading = true;
        while (i < l.size()) {
            const char c = l[i];
            if (c == ' ' && (leading || !leadingOnly)) {
                // A run of spaces: each part that reaches a tab stop becomes a
                // TAB when it is 2+ spaces, or a single space followed by more
                // blanks; spaces right before a TAB are absorbed by it.
                std::size_t j = i;
                while (j < l.size() && l[j] == ' ')
                    ++j;
                const bool tabFollows = j < l.size() && l[j] == '\t';
                std::size_t c0 = col;
                const std::size_t c1 = col + (j - i);
                while (c0 < c1) {
                    const std::size_t stop = (c0 / tab + 1) * tab;
                    if (stop > c1) {
                        if (!tabFollows)
                            out.append(c1 - c0, ' ');
                        c0 = c1;
                        break;
                    }
                    const std::size_t seg = stop - c0;
                    const bool followedByBlank = stop < c1 || tabFollows;
                    if (seg >= 2 || followedByBlank)
                        out += '\t';
                    else
                        out += ' ';
                    c0 = stop;
                }
                col = c1;
                i = j;
                continue;
            }
            if (c == '\t') {
                out += c;
                col = (col / tab + 1) * tab;
            } else {
                if (c != ' ')
                    leading = false;
                out += c;
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
                    ++col;
            }
            ++i;
        }
        return out;
    });
}

// ============================================================================
// Line operations
// ============================================================================

void removeEmptyLines(const Sci& sci, bool alsoBlank)
{
    sptr_t from = 0, to = sci.lineCount() - 1;
    const bool whole = sci(SCI_GETSELECTIONSTART) == sci(SCI_GETSELECTIONEND);
    if (!whole)
        std::tie(from, to) = selectionLines(sci);
    auto isEmpty = [&](sptr_t l) {
        const std::string t = sci.lineText(l);
        return alsoBlank ? t.find_first_not_of(" \t") == std::string::npos : t.empty();
    };
    sci(SCI_BEGINUNDOACTION);
    for (sptr_t l = to; l >= from; --l) {
        if (!isEmpty(l))
            continue;
        const sptr_t start = sci.lineStart(l);
        sptr_t end = sci.lineStart(l) + sci(SCI_LINELENGTH, static_cast<uptr_t>(l));
        sptr_t delFrom = start;
        if (l == sci.lineCount() - 1) {
            if (l == 0)
                continue;
            delFrom = sci.lineEnd(l - 1); // last line: remove the EOL before it
            end = sci.length();
        }
        sci.setTarget(delFrom, end);
        sci.replaceTarget({});
    }
    sci(SCI_ENDUNDOACTION);
}

void removeDuplicateLines(const Sci& sci, bool consecutiveOnly)
{
    sptr_t from = 0, to = sci.lineCount() - 1;
    if (sci(SCI_GETSELECTIONSTART) != sci(SCI_GETSELECTIONEND)) {
        std::tie(from, to) = selectionLines(sci);
        if (from == to)
            return;
    }
    sci(SCI_BEGINUNDOACTION);
    transformLines(sci, from, to, [consecutiveOnly](std::vector<std::string>& lines) {
        std::vector<std::string> out;
        out.reserve(lines.size());
        if (consecutiveOnly) {
            for (std::string& l : lines)
                if (out.empty() || out.back() != l)
                    out.push_back(std::move(l));
        } else {
            std::unordered_set<std::string> seen;
            for (std::string& l : lines)
                if (seen.insert(l).second)
                    out.push_back(std::move(l));
        }
        lines = std::move(out);
        return true;
    });
    sci(SCI_ENDUNDOACTION);
}

void insertBlankLine(const Sci& sci, bool above)
{
    // Upstream insertNewLineAbove/BelowCurrentLine: for every selection's line.
    const std::string eol = eolString(sci);
    const sptr_t n = sci(SCI_GETSELECTIONS);
    std::set<sptr_t> lines;
    for (sptr_t i = 0; i < n; ++i)
        lines.insert(sci.lineOf(sci(SCI_GETSELECTIONNCARET, static_cast<uptr_t>(i))));
    sci(SCI_BEGINUNDOACTION);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const sptr_t pos = above ? sci.lineStart(*it) : sci.lineEnd(*it);
        sci(SCI_INSERTTEXT, static_cast<uptr_t>(pos), reinterpret_cast<sptr_t>(eol.c_str()));
    }
    sci(SCI_ENDUNDOACTION);
    if (n == 1) {
        const sptr_t line = *lines.begin() + (above ? 0 : 1);
        sci(SCI_GOTOPOS, static_cast<uptr_t>(sci.lineStart(line)));
    }
}

void joinLines(const Sci& sci)
{
    const auto [l1, l2] = selectionLines(sci);
    if (l1 == l2)
        return;
    sci(SCI_SETSELECTION, static_cast<uptr_t>(sci.lineEnd(l2)), sci.lineStart(l1));
    sci(SCI_TARGETFROMSELECTION);
    sci(SCI_LINESJOIN);
}

void splitLines(const Sci& sci)
{
    if (sci(SCI_GETSELECTIONS) != 1)
        return;
    const auto [l1, l2] = selectionLines(sci);
    sci(SCI_SETSELECTION, static_cast<uptr_t>(sci.lineEnd(l2)), sci.lineStart(l1));
    sci(SCI_TARGETFROMSELECTION);
    if (sci(SCI_GETEDGEMODE) == EDGE_NONE) {
        sci(SCI_LINESSPLIT, 0);
    } else {
        const sptr_t w = sci(SCI_TEXTWIDTH, STYLE_DEFAULT, reinterpret_cast<sptr_t>("P"));
        sci(SCI_LINESSPLIT, static_cast<uptr_t>(w * (sci(SCI_GETEDGECOLUMN) + 1)));
    }
}

// ============================================================================
// Comments
// ============================================================================

bool lineComment(const Sci& sci, const std::string& tok, LineComment mode)
{
    if (tok.empty())
        return false;
    const auto [first, last] = selectionLines(sci);
    auto indentPos = [&](sptr_t l) { return sci(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(l)); };
    auto commented = [&](sptr_t l) {
        const sptr_t p = indentPos(l);
        return sci.lineEnd(l) - p >= static_cast<sptr_t>(tok.size()) && sci.range(p, p + static_cast<sptr_t>(tok.size())) == tok;
    };
    auto blank = [&](sptr_t l) { return indentPos(l) == sci.lineEnd(l); };

    bool remove = mode == LineComment::Remove;
    if (mode == LineComment::Toggle) {
        remove = true;
        for (sptr_t l = first; l <= last; ++l)
            if (!blank(l) && !commented(l))
                remove = false;
    }
    // The selection follows the text, as upstream, so toggling twice works.
    const sptr_t anchor = sci(SCI_GETANCHOR), caret = sci(SCI_GETCURRENTPOS);
    sptr_t lo = std::min(anchor, caret), hi = std::max(anchor, caret);
    sci(SCI_BEGINUNDOACTION);
    for (sptr_t l = first; l <= last; ++l) {
        if (blank(l))
            continue;
        const sptr_t p = indentPos(l);
        sptr_t delta = 0;
        if (remove) {
            if (!commented(l))
                continue;
            sptr_t n = static_cast<sptr_t>(tok.size());
            if (p + n < sci.lineEnd(l) && sci(SCI_GETCHARAT, static_cast<uptr_t>(p + n)) == ' ')
                ++n;
            sci(SCI_DELETERANGE, static_cast<uptr_t>(p), n);
            delta = -n;
        } else {
            const std::string ins = tok + " ";
            sci(SCI_INSERTTEXT, static_cast<uptr_t>(p), reinterpret_cast<sptr_t>(ins.c_str()));
            delta = static_cast<sptr_t>(ins.size());
        }
        if (p < lo)
            lo = std::max(p, lo + delta);
        if (p < hi)
            hi = std::max(p, hi + delta);
    }
    sci(SCI_ENDUNDOACTION);
    if (anchor <= caret)
        sci(SCI_SETSEL, static_cast<uptr_t>(lo), hi);
    else
        sci(SCI_SETSEL, static_cast<uptr_t>(hi), lo);
    return true;
}

bool streamComment(const Sci& sci, const std::string& start, const std::string& end, bool add)
{
    if (start.empty() || end.empty())
        return false;
    sptr_t s = sci(SCI_GETSELECTIONSTART), e = sci(SCI_GETSELECTIONEND);
    const std::string open = start + " ", close = " " + end;
    if (add) {
        if (s == e) { // no selection: the word at the caret, as upstream
            s = sci(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(s), true);
            e = sci(SCI_WORDENDPOSITION, static_cast<uptr_t>(e), true);
        }
        sci(SCI_BEGINUNDOACTION);
        sci(SCI_INSERTTEXT, static_cast<uptr_t>(e), reinterpret_cast<sptr_t>(close.c_str()));
        sci(SCI_INSERTTEXT, static_cast<uptr_t>(s), reinterpret_cast<sptr_t>(open.c_str()));
        sci(SCI_ENDUNDOACTION);
        sci(SCI_SETSEL, static_cast<uptr_t>(s), e + static_cast<sptr_t>(open.size() + close.size()));
        return true;
    }
    // Uncomment: the innermost start..end pair around the caret or selection.
    const sptr_t len = sci.length();
    const std::string before = sci.range(0, s);
    const std::size_t o = before.rfind(start);
    if (o == std::string::npos)
        return false;
    const std::string after = sci.range(static_cast<sptr_t>(o), len);
    const std::size_t c = after.find(end, start.size());
    if (c == std::string::npos || static_cast<sptr_t>(o + c + end.size()) < e)
        return false;
    sptr_t cs = static_cast<sptr_t>(o + c), ce = cs + static_cast<sptr_t>(end.size());
    if (cs > 0 && sci(SCI_GETCHARAT, static_cast<uptr_t>(cs - 1)) == ' ')
        --cs;
    sptr_t os = static_cast<sptr_t>(o), oe = os + static_cast<sptr_t>(start.size());
    if (oe < cs && sci(SCI_GETCHARAT, static_cast<uptr_t>(oe)) == ' ')
        ++oe;
    sci(SCI_BEGINUNDOACTION);
    sci(SCI_DELETERANGE, static_cast<uptr_t>(cs), ce - cs);
    sci(SCI_DELETERANGE, static_cast<uptr_t>(os), oe - os);
    sci(SCI_ENDUNDOACTION);
    return true;
}

// ============================================================================
// Column Editor
// ============================================================================

namespace {

struct Slot {
    sptr_t start = 0, end = 0; // replaced range
    sptr_t vspace = 0;         // virtual space before `start` to fill with spaces
};

// Where column text goes: the selections of a column / multiple selection,
// else the caret column from the caret line down. In document order.
std::vector<Slot> columnSlots(const Sci& sci, bool& selections)
{
    std::vector<Slot> slots;
    selections = sci(SCI_SELECTIONISRECTANGLE) || sci(SCI_GETSELECTIONS) > 1;
    if (selections) {
        for (sptr_t i = 0, n = sci(SCI_GETSELECTIONS); i < n; ++i)
            slots.push_back({sci(SCI_GETSELECTIONNSTART, static_cast<uptr_t>(i)), sci(SCI_GETSELECTIONNEND, static_cast<uptr_t>(i)),
                             sci(SCI_GETSELECTIONNSTARTVIRTUALSPACE, static_cast<uptr_t>(i))});
        std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.start < b.start; });
        return slots;
    }
    const sptr_t caret = sci(SCI_GETCURRENTPOS);
    const sptr_t col = sci(SCI_GETCOLUMN, static_cast<uptr_t>(caret));
    for (sptr_t l = sci.lineOf(caret), last = sci.lineCount() - 1; l <= last; ++l) {
        const sptr_t end = sci.lineEnd(l);
        const sptr_t endCol = sci(SCI_GETCOLUMN, static_cast<uptr_t>(end));
        if (endCol < col)
            slots.push_back({end, end, col - endCol});
        else {
            const sptr_t p = sci(SCI_FINDCOLUMN, static_cast<uptr_t>(l), col);
            slots.push_back({p, p, 0});
        }
    }
    return slots;
}

void columnReplace(const Sci& sci, const std::vector<std::string>& texts)
{
    bool selections = false;
    std::vector<Slot> slots = columnSlots(sci, selections);
    sci(SCI_BEGINUNDOACTION);
    sptr_t delta = 0;
    std::vector<std::pair<sptr_t, sptr_t>> done;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const std::string& t = texts[std::min(i, texts.size() - 1)];
        const sptr_t start = slots[i].start + delta, end = slots[i].end + delta;
        const std::string ins = std::string(static_cast<std::size_t>(slots[i].vspace), ' ') + t;
        sci.setTarget(start, end);
        sci.replaceTarget(ins);
        delta += static_cast<sptr_t>(ins.size()) - (end - start);
        done.emplace_back(start + slots[i].vspace, start + static_cast<sptr_t>(ins.size()));
    }
    sci(SCI_ENDUNDOACTION);
    // Upstream selects what was inserted, so it can be edited further.
    if (selections && !done.empty()) {
        sci(SCI_SETSELECTION, static_cast<uptr_t>(done[0].second), done[0].first);
        for (std::size_t i = 1; i < done.size(); ++i)
            sci(SCI_ADDSELECTION, static_cast<uptr_t>(done[i].second), done[i].first);
    }
}

} // namespace

std::string formatColumnNumber(std::uint64_t n, NumberBase base, std::size_t width, Leading lead, bool upperHex)
{
    const int radix = base == NumberBase::Hex ? 16 : base == NumberBase::Oct ? 8 : base == NumberBase::Bin ? 2 : 10;
    const char* digits = upperHex ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), digits[n % static_cast<std::uint64_t>(radix)]);
        n /= static_cast<std::uint64_t>(radix);
    } while (n);
    if (s.size() >= width)
        return s;
    const std::size_t pad = width - s.size();
    switch (lead) {
    case Leading::Zeros: return std::string(pad, '0') + s;
    case Leading::Spaces: return std::string(pad, ' ') + s;
    case Leading::None: break;
    }
    return s + std::string(pad, ' ');
}

void columnInsertText(const Sci& sci, const std::string& text)
{
    columnReplace(sci, {text});
}

bool columnInsertNumbers(const Sci& sci, std::uint64_t initial, std::uint64_t incr, std::uint64_t repeat, NumberBase base, Leading lead,
                         bool upperHex)
{
    if (repeat == 0)
        return false;
    bool selections = false;
    const std::size_t count = columnSlots(sci, selections).size();
    std::vector<std::uint64_t> nums;
    for (std::uint64_t cur = initial; nums.size() < count; cur += incr)
        for (std::uint64_t r = 0; r < repeat && nums.size() < count; ++r)
            nums.push_back(cur);
    if (nums.empty())
        return true;
    std::size_t width = 0;
    for (std::uint64_t v : {initial, nums.back()})
        width = std::max(width, formatColumnNumber(v, base, 0, Leading::None).size());
    std::vector<std::string> texts;
    for (std::uint64_t v : nums)
        texts.push_back(formatColumnNumber(v, base, width, lead, upperHex));
    columnReplace(sci, texts);
    return true;
}

// ============================================================================
// Bookmarked lines
// ============================================================================

std::vector<sptr_t> markedLines(const Sci& sci, int marker)
{
    std::vector<sptr_t> out;
    for (sptr_t l = sci(SCI_MARKERNEXT, 0, 1 << marker); l >= 0; l = sci(SCI_MARKERNEXT, static_cast<uptr_t>(l + 1), 1 << marker))
        out.push_back(l);
    return out;
}

std::string copyMarkedLines(const Sci& sci, int marker)
{
    std::string out;
    for (sptr_t l : markedLines(sci, marker)) {
        const sptr_t s = sci.lineStart(l);
        out += sci.range(s, s + sci(SCI_LINELENGTH, static_cast<uptr_t>(l)));
    }
    return out;
}

void deleteMarkedLines(const Sci& sci, int marker, bool marked)
{
    sci(SCI_BEGINUNDOACTION);
    for (sptr_t l = sci.lineCount() - 1; l >= 0; --l) {
        const bool has = (sci(SCI_MARKERGET, static_cast<uptr_t>(l)) & (1 << marker)) != 0;
        if (has != marked)
            continue;
        if (has)
            sci(SCI_MARKERDELETE, static_cast<uptr_t>(l), marker);
        const sptr_t s = sci.lineStart(l);
        sci.setTarget(s, s + sci(SCI_LINELENGTH, static_cast<uptr_t>(l)));
        sci.replaceTarget({});
    }
    sci(SCI_ENDUNDOACTION);
}

void replaceMarkedLines(const Sci& sci, int marker, const std::string& text)
{
    const std::vector<sptr_t> lines = markedLines(sci, marker);
    sci(SCI_BEGINUNDOACTION);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        sci.setTarget(sci.lineStart(*it), sci.lineEnd(*it));
        sci.replaceTarget(text);
    }
    sci(SCI_ENDUNDOACTION);
}

void inverseMarks(const Sci& sci, int marker)
{
    for (sptr_t l = 0, n = sci.lineCount(); l < n; ++l) {
        if (sci(SCI_MARKERGET, static_cast<uptr_t>(l)) & (1 << marker))
            sci(SCI_MARKERDELETE, static_cast<uptr_t>(l), marker);
        else
            sci(SCI_MARKERADD, static_cast<uptr_t>(l), marker);
    }
}

} // namespace npp::core
