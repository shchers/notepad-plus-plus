// "Search results" panel — upstream Finder (FindReplaceDlg.h): a read-only
// Scintilla coloured by Lexilla's searchResult lexer, in upstream's text
// format, so it looks and folds the same:
//
//   Search "foo" (3 hits in 2 files of 5 searched) [Normal]
//     /path/a.txt (2 hits)
//   	Line 4: a foo line
//   	Line 9: foo and foo           <- one entry per line, all hits coloured
//     /path/b.txt (1 hit)
//   	Line 1: foo
//
// The newest search goes on top and older ones are folded. Double-click (or
// F4 / Shift+F4 through next()) opens a result. It is a docking panel
// (bottom by default), whose caption gives the title and the close button.
#pragma once

#include "core/SearchEngine.h"
#include "core/Translator.h"
#include "ui/EditorView.h"

#include <gtk/gtk.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace npp::ui {

struct ResultLocation {
    std::string file;      // Buffer::fullName(): path, or "new 1" for untitled
    sptr_t line = -1;      // 0-based; -1 = the file itself (file header line)
    sptr_t startInLine = 0; // byte offsets of the first hit on the line
    sptr_t endInLine = 0;
};

class SearchResults {
public:
    explicit SearchResults(const core::Translator& tr);
    ~SearchResults();
    SearchResults(const SearchResults&) = delete;
    SearchResults& operator=(const SearchResults&) = delete;

    GtkWidget* widget() const { return box_; }
    void applyStyles(const core::StyleCatalog& styles);

    // One search: begin(), addFile() for each file with hits, finish().
    void begin(const std::string& what);
    void addFile(const std::string& file, const std::vector<core::Hit>& hits);
    bool oneEntryPerLine = true; // Preferences > Searching
    // `searched` = number of files (or selections) searched.
    void finish(int searched, bool wholeNotSelection, const core::FindOptions& o);
    void clear();

    // Moves to the next/previous result line and opens it. False if none.
    bool next(bool forward);
    bool empty() const { return infos_.empty(); }
    void focus();
    std::string text() const { return view_.text(); }
    int lastHits() const { return lastHits_; }

    std::function<void(const ResultLocation&)> onOpen;

private:
    struct Pending {
        std::string text;
        std::vector<SearchResultMarkingLine> marks;
        std::vector<std::optional<ResultLocation>> infos;
        int hits = 0;
        int files = 0;
    };
    void openLine(sptr_t line);
    void updateMarkings();
    std::string hitsString(int n) const;
    bool english() const { return tr_.fileName() == "english.xml"; }

    const core::Translator& tr_;
    EditorView view_;
    GtkWidget* box_ = nullptr;
    Pending pending_;
    std::vector<SearchResultMarkingLine> marks_; // per document line, for the lexer
    SearchResultMarkings markStruct_{0, nullptr};
    std::vector<std::optional<ResultLocation>> infos_; // per document line
    int lastHits_ = 0;
};

} // namespace npp::ui
