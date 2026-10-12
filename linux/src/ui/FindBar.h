// Find / Replace — inline bar for the skeleton. Upstream has a full
// FindReplaceDlg (Find in Files, Mark, Count, Find in all docs); the search
// primitives below are the part that ports directly, since they only talk
// to Scintilla through execute().
#pragma once

#include "ui/EditorView.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace npp::ui {

struct SearchOptions {
    std::string pattern;
    bool matchCase = false;
    bool wholeWord = false;
    bool regex = false; // Boost regex engine, same syntax as Notepad++ on Windows
    bool wrap = true;

    // Upstream FindReplaceDlg picks Boost empty-match semantics per operation.
    enum class Op { FindNext, VerifyForReplace, ReplaceAll };
    int scintillaFlags(Op op = Op::FindNext) const;
};

enum class SearchResult { Found, Wrapped, NotFound, InvalidRegex };

// Selects the next/previous match starting at the current selection.
SearchResult findNext(EditorView& view, const SearchOptions& opts, bool forward);
// Replaces the current selection if it is a match, then finds the next one.
SearchResult replaceNext(EditorView& view, const SearchOptions& opts, const std::string& replacement);
// Replaces every match in the document as one undo step. Returns the count, or -1 on invalid regex.
// Regex replacement uses Boost "format_all" syntax exactly like Notepad++ on
// Windows: \1 or $1 for groups, and literal ( ) must be escaped as \( \).
int replaceAll(EditorView& view, const SearchOptions& opts, const std::string& replacement);

class FindBar {
public:
    // `activeView` returns the editor the bar should operate on (may be null).
    explicit FindBar(std::function<EditorView*()> activeView);

    GtkWidget* widget() const { return bar_; }

    void show(bool withReplace);   // reveal, prefill with selection, focus entry
    void hide();
    bool isVisible() const;
    void findAgain(bool forward);   // F3 / Shift+F3

private:
    SearchOptions options() const;
    void report(SearchResult r, int replaced = -1);
    void doFind(bool forward);

    std::function<EditorView*()> activeView_;
    GtkWidget* bar_ = nullptr;
    GtkWidget* findEntry_ = nullptr;
    GtkWidget* replaceEntry_ = nullptr;
    GtkWidget* replaceRow_ = nullptr;
    GtkWidget* matchCase_ = nullptr;
    GtkWidget* wholeWord_ = nullptr;
    GtkWidget* regex_ = nullptr;
    GtkWidget* status_ = nullptr;
};

} // namespace npp::ui
