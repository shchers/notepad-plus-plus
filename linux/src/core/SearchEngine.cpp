#include "core/SearchEngine.h"

#include <BoostRegexSearch.h> // Notepad++ SCFIND_REGEXP_* extension flags

#include <glib.h>

#include <algorithm>

namespace npp::core {

namespace {

constexpr sptr_t kHitLineMax = 1024; // upstream SC_SEARCHRESULT_LINEBUFFERMAXLENGTH

void appendUtf8(std::string& out, unsigned cp)
{
    char buf[8];
    const int n = g_unichar_to_utf8(static_cast<gunichar>(cp), buf);
    out.append(buf, static_cast<std::size_t>(n));
}

bool readBase(std::string_view s, int base, std::size_t size, unsigned& value)
{
    if (s.size() < size)
        return false;
    value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const int d = g_ascii_xdigit_value(s[i]);
        if (d < 0 || d >= base)
            return false;
        value = value * static_cast<unsigned>(base) + static_cast<unsigned>(d);
    }
    return true;
}

} // namespace

std::string convertExtended(std::string_view q)
{
    std::string out;
    for (std::size_t i = 0; i < q.size(); ++i) {
        const char c = q[i];
        if (c != '\\' || i + 1 >= q.size()) {
            out += c;
            continue;
        }
        const char e = q[++i];
        switch (e) {
        case 'r': out += '\r'; continue;
        case 'n': out += '\n'; continue;
        case '0': out += '\0'; continue;
        case 't': out += '\t'; continue;
        case '\\': out += '\\'; continue;
        case 'b': case 'd': case 'o': case 'x': case 'u': {
            const int base = e == 'b' ? 2 : e == 'o' ? 8 : e == 'd' ? 10 : 16;
            const std::size_t size = e == 'b' ? 8 : e == 'x' ? 2 : e == 'u' ? 4 : 3;
            unsigned v = 0;
            if (readBase(q.substr(i + 1), base, size, v)) {
                if (v < 0x80)
                    out += static_cast<char>(v);
                else
                    appendUtf8(out, v);
                i += size;
                continue;
            }
            break; // not enough digits: keep as text
        }
        default:
            break;
        }
        out += '\\';
        out += e;
    }
    return out;
}

int baseSearchFlags(const FindOptions& o)
{
    int f = 0;
    if (o.matchCase)
        f |= SCFIND_MATCHCASE;
    if (o.wholeWord && o.mode != SearchMode::Regex)
        f |= SCFIND_WHOLEWORD;
    if (o.mode == SearchMode::Regex) {
        f |= SCFIND_REGEXP | SCFIND_POSIX;
        if (o.dotMatchesNewline)
            f |= SCFIND_REGEXP_DOTMATCHESNL;
    }
    return f;
}

std::string SearchEngine::pattern(const FindOptions& o)
{
    return o.mode == SearchMode::Extended ? convertExtended(o.what) : o.what;
}

int SearchEngine::flags(const FindOptions& o, Op op) const
{
    int f = baseSearchFlags(o) | SCFIND_REGEXP_SKIPCRLFASONE;
    switch (op) {
    case Op::FindNext: f |= SCFIND_REGEXP_EMPTYMATCH_ALL; break;
    case Op::VerifyForReplace: f |= SCFIND_REGEXP_EMPTYMATCH_ALL | SCFIND_REGEXP_EMPTYMATCH_ALLOWATSTART; break;
    case Op::AllOperations: f |= SCFIND_REGEXP_EMPTYMATCH_NOTAFTERMATCH; break;
    }
    return f;
}

sptr_t SearchEngine::searchInTarget(const std::string& pat, sptr_t from, sptr_t to) const
{
    sci_.setTarget(from, to);
    return sci_(SCI_SEARCHINTARGET, pat.size(), reinterpret_cast<sptr_t>(pat.data()));
}

FindStatus SearchEngine::findNext(const FindOptions& o) const
{
    const std::string pat = pattern(o);
    if (pat.empty())
        return FindStatus::Empty;
    sci_(SCI_SETSEARCHFLAGS, static_cast<uptr_t>(flags(o, Op::FindNext)));
    const sptr_t len = sci_.length();
    const sptr_t selStart = sci_(SCI_GETSELECTIONSTART), selEnd = sci_(SCI_GETSELECTIONEND);

    auto select = [&] {
        const sptr_t a = sci_(SCI_GETTARGETSTART), b = sci_(SCI_GETTARGETEND);
        sci_(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(sci_.lineOf(a)));
        if (o.forward)
            sci_(SCI_SETSEL, static_cast<uptr_t>(a), b);
        else
            sci_(SCI_SETSEL, static_cast<uptr_t>(b), a);
        sci_(SCI_SCROLLRANGE, static_cast<uptr_t>(b), a);
    };
    sptr_t pos = o.forward ? searchInTarget(pat, selEnd, len) : searchInTarget(pat, selStart, 0);
    if (pos == -2)
        return FindStatus::InvalidRegex;
    if (pos >= 0) {
        select();
        return FindStatus::Found;
    }
    if (!o.wrap)
        return FindStatus::NotFound;
    pos = o.forward ? searchInTarget(pat, 0, len) : searchInTarget(pat, len, 0);
    if (pos < 0)
        return pos == -2 ? FindStatus::InvalidRegex : FindStatus::NotFound;
    select();
    return FindStatus::Wrapped;
}

FindStatus SearchEngine::replace(const FindOptions& o, bool* replaced) const
{
    if (replaced)
        *replaced = false;
    const std::string pat = pattern(o);
    if (pat.empty())
        return FindStatus::Empty;
    const sptr_t a = sci_(SCI_GETSELECTIONSTART), b = sci_(SCI_GETSELECTIONEND);
    // Replace the selection first when it is exactly a match.
    sci_(SCI_SETSEARCHFLAGS, static_cast<uptr_t>(flags(o, Op::VerifyForReplace)));
    const sptr_t m = searchInTarget(pat, a, b);
    if (m == -2)
        return FindStatus::InvalidRegex;
    if (m == a && sci_(SCI_GETTARGETEND) == b) {
        const std::string with = o.mode == SearchMode::Extended ? convertExtended(o.with) : o.with;
        const unsigned msg = o.mode == SearchMode::Regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET;
        sci_(msg, with.size(), reinterpret_cast<sptr_t>(with.data()));
        const sptr_t start = sci_(SCI_GETTARGETSTART), end = sci_(SCI_GETTARGETEND);
        // Continue after (or, going up, before) the replacement.
        const sptr_t caret = o.forward ? end : start;
        sci_(SCI_SETSEL, static_cast<uptr_t>(caret), caret);
        if (replaced)
            *replaced = true;
    }
    return findNext(o);
}

void SearchEngine::scope(const FindOptions& o, bool countOp, bool allowSelection, sptr_t& from, sptr_t& to) const
{
    // Upstream processAll: direction, then wrap / whole document, then the
    // selection when "In selection" applies.
    const sptr_t len = sci_.length();
    const sptr_t selStart = sci_(SCI_GETSELECTIONSTART), selEnd = sci_(SCI_GETSELECTIONEND);
    if (o.forward) {
        from = selStart;
        to = len;
    } else {
        from = 0;
        to = selEnd;
    }
    if (countOp && o.inSelection) {
        from = selStart;
        to = selEnd;
    } else if (o.wrap) {
        from = 0;
        to = len;
    }
    if (allowSelection && o.inSelection) {
        from = selStart;
        to = selEnd;
    }
}

int SearchEngine::replaceAll(const FindOptions& o) const
{
    if (sci_(SCI_GETREADONLY))
        return 0;
    sptr_t from = 0, to = 0;
    scope(o, false, true, from, to);
    const bool regex = o.mode == SearchMode::Regex;
    const std::string with = o.mode == SearchMode::Extended ? convertExtended(o.with) : o.with;
    const sptr_t selStart = from, oldLen = sci_.length();
    sci_(SCI_BEGINUNDOACTION);
    const int n = forEachMatch(o, from, to, [&](sptr_t, sptr_t) -> sptr_t {
        return sci_(regex ? SCI_REPLACETARGETRE : SCI_REPLACETARGET, with.size(), reinterpret_cast<sptr_t>(with.data()));
    });
    sci_(SCI_ENDUNDOACTION);
    if (n > 0 && o.inSelection) // keep the (resized) selection, as upstream
        sci_(SCI_SETSELECTION, static_cast<uptr_t>(to + sci_.length() - oldLen), selStart);
    return n;
}

int SearchEngine::countAll(const FindOptions& o) const
{
    sptr_t from = 0, to = 0;
    scope(o, true, false, from, to);
    return forEachMatch(o, from, to, [](sptr_t a, sptr_t b) { return b - a; });
}

int SearchEngine::markAll(const FindOptions& o, int indicator, int bookmarkMarker) const
{
    if (o.purge) {
        clearMarks(indicator);
        if (bookmarkMarker >= 0 && o.bookmarkLine)
            sci_(SCI_MARKERDELETEALL, static_cast<uptr_t>(bookmarkMarker));
    }
    sptr_t from = 0, to = 0;
    scope(o, false, true, from, to);
    sci_(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator));
    return forEachMatch(o, from, to, [&](sptr_t a, sptr_t b) -> sptr_t {
        if (b > a)
            sci_(SCI_INDICATORFILLRANGE, static_cast<uptr_t>(a), b - a);
        if (bookmarkMarker >= 0 && o.bookmarkLine) {
            const sptr_t first = sci_.lineOf(a), last = b > a ? sci_.lineOf(b - 1) : first;
            for (sptr_t l = first; l <= last; ++l)
                if (!(sci_(SCI_MARKERGET, static_cast<uptr_t>(l)) & (1 << bookmarkMarker)))
                    sci_(SCI_MARKERADD, static_cast<uptr_t>(l), bookmarkMarker);
        }
        return b - a;
    });
}

void SearchEngine::clearMarks(int indicator) const
{
    sci_(SCI_SETINDICATORCURRENT, static_cast<uptr_t>(indicator));
    sci_(SCI_INDICATORCLEARRANGE, 0, sci_.length());
}

int SearchEngine::findAll(const FindOptions& o, std::vector<Hit>& hits, bool wholeDocument) const
{
    sptr_t from = 0, to = sci_.length();
    if (!wholeDocument && o.inSelection) {
        from = sci_(SCI_GETSELECTIONSTART);
        to = sci_(SCI_GETSELECTIONEND);
    }
    return forEachMatch(o, from, to, [&](sptr_t a, sptr_t b) -> sptr_t {
        Hit h;
        h.line = sci_.lineOf(a);
        h.start = a;
        h.end = b;
        const sptr_t ls = sci_.lineStart(h.line);
        const sptr_t le = std::min(sci_.lineEnd(h.line), ls + kHitLineMax);
        h.lineText = sci_.range(ls, le);
        h.matchStartInLine = std::min(a - ls, le - ls);
        h.matchEndInLine = std::min(b - ls, le - ls);
        hits.push_back(std::move(h));
        return b - a;
    });
}

} // namespace npp::core
