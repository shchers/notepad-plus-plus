// Edit menu operations — upstream Notepad_plus / ScintillaEditView code for
// Edit > Convert Case, Line Operations, Blank Operations, Comment/Uncomment
// and Search > Bookmark line operations, written against core::Sci so they
// run without GTK. Each operation is one undo step.
//
// Scope follows upstream: an empty selection means the whole document, a
// selection is extended to whole lines where upstream does so.
#pragma once

#include "core/Sci.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace npp::core {

// ---- case (upstream ScintillaEditView::changeCase) -------------------------
enum class TextCase { Upper, Lower, ProperForce, ProperBlend, SentenceForce, SentenceBlend, Invert, Random };
std::string convertCase(std::string_view utf8, TextCase c);
// Converts each selection (rectangular / multiple too); nothing without one.
void convertSelectionCase(const Sci& sci, TextCase c);

// ---- sorting (upstream MISC/Common/Sorters.h) ------------------------------
enum class SortKind { Lexicographic, LexicographicIgnoreCase, Locale, Integer, DecimalComma, DecimalDot, Length, Reverse, Random };
// Sorts `lines` in place. Columns [fromCol, toCol) in characters select the
// sort key (toCol 0 = whole line). Numeric sorts fail on a line that is not a
// number: returns false with `*badLine` set to its index.
bool sortStrings(std::vector<std::string>& lines, SortKind kind, bool descending, std::size_t fromCol = 0,
                 std::size_t toCol = 0, std::size_t* badLine = nullptr);
// Sorts the selected lines (or the rectangle's lines by its columns, or the
// whole document). False when nothing to sort or a numeric conversion fails.
bool sortLines(const Sci& sci, SortKind kind, bool descending, std::string* error = nullptr);

// ---- blank operations ------------------------------------------------------
enum class Trim { Leading, Trailing, Both };
void trim(const Sci& sci, Trim which);
void eolToSpace(const Sci& sci);
void trimAll(const Sci& sci); // Trim both and EOL to space
void tabsToSpaces(const Sci& sci);
void spacesToTabs(const Sci& sci, bool leadingOnly);

// ---- line operations -------------------------------------------------------
void removeEmptyLines(const Sci& sci, bool alsoBlank);
void removeDuplicateLines(const Sci& sci, bool consecutiveOnly);
void insertBlankLine(const Sci& sci, bool above);
void joinLines(const Sci& sci);
void splitLines(const Sci& sci);

// ---- comments --------------------------------------------------------------
enum class LineComment { Toggle, Add, Remove };
// `token` is the language's commentLine; returns false if it has none.
bool lineComment(const Sci& sci, const std::string& token, LineComment mode);
// Block comment / uncomment with commentStart/commentEnd around the selection.
bool streamComment(const Sci& sci, const std::string& start, const std::string& end, bool add);

// ---- Column Editor (upstream ColumnEditorDlg, ScintillaEditView::columnReplace)
enum class NumberBase { Dec, Hex, Oct, Bin };
enum class Leading { None, Zeros, Spaces }; // None: left-aligned, padded after
// `n` in `base`, padded to `width` characters.
std::string formatColumnNumber(std::uint64_t n, NumberBase base, std::size_t width, Leading lead, bool upperHex = false);
// Into each selection of a column (rectangular) or multiple selection, or
// else at the caret's column on every line from the caret line down;
// short lines are padded with spaces. One undo step.
void columnInsertText(const Sci& sci, const std::string& text);
// Numbers initial, initial+incr, ... each repeated `repeat` times, all the
// same width. False if repeat is 0.
bool columnInsertNumbers(const Sci& sci, std::uint64_t initial, std::uint64_t incr, std::uint64_t repeat, NumberBase base,
                         Leading lead, bool upperHex = false);

// ---- bookmarked lines (Search > Bookmark) ----------------------------------
std::vector<sptr_t> markedLines(const Sci& sci, int marker);
std::string copyMarkedLines(const Sci& sci, int marker);   // with their line endings
void deleteMarkedLines(const Sci& sci, int marker, bool marked); // false = the unmarked ones
void replaceMarkedLines(const Sci& sci, int marker, const std::string& text); // "Paste to (Replace)"
void inverseMarks(const Sci& sci, int marker);

} // namespace npp::core
