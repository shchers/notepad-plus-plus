// Search engine — the Win32-free part of upstream FindReplaceDlg
// (processFindNext, processReplace, processAll/processRange, Searching::*).
// Runs on any Scintilla through core::Sci with Notepad++'s Boost regex
// engine, choosing the same SCFIND_REGEXP_EMPTYMATCH_* behaviour per
// operation as upstream, so "^", lookbehind and empty matches behave alike.
#pragma once

#include "core/Sci.h"

#include <string>
#include <string_view>
#include <vector>

namespace npp::core {

// Upstream SearchType.
enum class SearchMode { Normal = 0, Extended = 1, Regex = 2 };

struct FindOptions {
    std::string what;
    std::string with;              // replacement
    SearchMode mode = SearchMode::Normal;
    bool matchCase = false;
    bool wholeWord = false;
    bool dotMatchesNewline = false; // regex only
    bool wrap = true;
    bool forward = true;           // upstream _whichDirection (DIR_DOWN)
    bool inSelection = false;
    // Mark tab
    bool bookmarkLine = false;
    bool purge = false;
    // Find in Files tab
    std::string directory;
    std::string filters;
    bool recursive = true;
    bool hidden = false;
};

enum class FindStatus { Found, Wrapped, NotFound, InvalidRegex, Empty };

// Upstream Searching::convertExtendedToString: \n \r \t \0 \\ and \bNNNNNNNN
// \oNNN \dNNN \xNN \uNNNN. Numeric escapes give the Unicode character of that
// value, encoded as UTF-8 (the document encoding inside Scintilla). Unknown
// escapes stay as written.
std::string convertExtended(std::string_view s);

// Upstream Searching::buildSearchFlags (without the empty-match policy).
int baseSearchFlags(const FindOptions& o);

struct Match {
    sptr_t start = 0;
    sptr_t end = 0;
};

// One line of Find All output.
struct Hit {
    sptr_t line = 0;               // 0-based
    sptr_t start = 0;              // match position in the document
    sptr_t end = 0;
    std::string lineText;          // the line without its EOL (cut at 1024 bytes)
    sptr_t matchStartInLine = 0;   // byte offsets of the match in lineText
    sptr_t matchEndInLine = 0;
};

// Indicators and marker used by Mark (upstream SCE_UNIVERSAL_FOUND_STYLE and
// MARK_BOOKMARK: the same numbers as stylers.xml "Find Mark Style" styleID).
inline constexpr int kFoundIndicator = 31;
inline constexpr int kIncrementalIndicator = 28;
// "Mark Style 1".."5" (Style All Occurrences of Token): styleIDs 25..21.
inline constexpr int markStyleIndicator(int n) { return 26 - n; }

class SearchEngine {
public:
    explicit SearchEngine(Sci sci) : sci_(sci) {}

    // Find Next / Previous from the selection; selects the match.
    FindStatus findNext(const FindOptions& o) const;
    // Upstream processReplace: replaces the selection when it is a match, then
    // finds the next one. `*replaced` tells whether the selection was replaced.
    // `moveToNext` false: the replacement stays selected (Preferences >
    // Searching "Don't move to the following occurrence").
    FindStatus replace(const FindOptions& o, bool* replaced = nullptr, bool moveToNext = true) const;

    // Upstream processAll scopes: whole document, or the selection when
    // `inSelection` is set (and wrap/direction otherwise). One undo step.
    // Return the number processed, -1 for an invalid regex.
    int replaceAll(const FindOptions& o) const;
    int countAll(const FindOptions& o) const;
    int markAll(const FindOptions& o, int indicator = kFoundIndicator, int bookmarkMarker = -1) const;
    int findAll(const FindOptions& o, std::vector<Hit>& hits, bool wholeDocument = false) const;
    void clearMarks(int indicator = kFoundIndicator) const;

    // Raw range iteration (upstream processRange): `fn` is called for each
    // match and returns the length the match has after processing (for
    // replacements); stops when it returns < 0.
    template <class Fn>
    int forEachMatch(const FindOptions& o, sptr_t from, sptr_t to, Fn&& fn) const;

    // Pattern actually sent to Scintilla (extended escapes resolved).
    static std::string pattern(const FindOptions& o);

private:
    enum class Op { FindNext, VerifyForReplace, AllOperations };
    int flags(const FindOptions& o, Op op) const;
    sptr_t searchInTarget(const std::string& pat, sptr_t from, sptr_t to) const;
    void scope(const FindOptions& o, bool countOp, bool allowSelection, sptr_t& from, sptr_t& to) const;

    Sci sci_;
};

// ---------------------------------------------------------------------------

template <class Fn>
int SearchEngine::forEachMatch(const FindOptions& o, sptr_t from, sptr_t to, Fn&& fn) const
{
    const std::string pat = pattern(o);
    if (pat.empty() || from == to)
        return 0;
    sci_(SCI_SETSEARCHFLAGS, static_cast<uptr_t>(flags(o, Op::AllOperations)));
    int n = 0;
    for (;;) {
        const sptr_t start = searchInTarget(pat, from, to);
        if (start == -2)
            return -1;
        if (start < 0)
            break;
        const sptr_t end = sci_(SCI_GETTARGETEND);
        if (end > to)
            break;
        const sptr_t newLen = fn(start, end);
        if (newLen < 0)
            break;
        ++n;
        const sptr_t delta = newLen - (end - start);
        if (start + (end - start) == to)
            break;
        from = start + newLen;
        to += delta;
        if (from > to)
            break;
    }
    return n;
}

} // namespace npp::core
