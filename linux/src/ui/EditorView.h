// GTK counterpart of upstream ScintillaEditView.
//
// The important seam: execute(msg, w, l) has the same shape and the same
// implementation strategy (Scintilla direct function pointer) as upstream's
// ScintillaEditView::execute(). Upstream code calls execute(SCI_...) ~1900
// times; keeping this signature lets that code move over with type changes
// only (WPARAM/LPARAM -> uptr_t/sptr_t), not rewrites.
#pragma once

#include "core/LanguageCatalog.h"
#include "core/StyleCatalog.h"

// Order matters: ScintillaWidget.h expects GTK types to be declared.
#include <gtk/gtk.h>
#include <Scintilla.h>
#include <ScintillaWidget.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace npp::ui {

// Caret, selection and scroll state — upstream Position (saved per buffer and
// view, and in session.xml).
struct ViewPosition {
    sptr_t firstVisibleLine = 0; // display line
    sptr_t xOffset = 0;
    sptr_t startPos = 0;         // anchor
    sptr_t endPos = 0;           // caret
    sptr_t selMode = 0;          // SC_SEL_*
    sptr_t scrollWidth = 1;
};

class EditorView {
public:
    EditorView();
    ~EditorView();
    EditorView(const EditorView&) = delete;
    EditorView& operator=(const EditorView&) = delete;

    GtkWidget* widget() const { return widget_; }

    sptr_t execute(unsigned int msg, uptr_t wParam = 0, sptr_t lParam = 0) const
    {
        return directFn_(directPtr_, msg, wParam, lParam);
    }

    // Applies global styles (default font, caret line, selection, margins)
    // and then the language's lexer, keywords and styles. `langs` is needed
    // because some lexers pull keywords from other languages (embedded JS in
    // HTML, Doxygen keywords from C++).
    void applyLanguage(const core::Language& lang, const core::LanguageCatalog& langs, const core::StyleCatalog& styles);
    const core::Language* language() const { return lang_; }

    std::string text() const;
    void setText(std::string_view utf8);       // replaces content, clears undo, sets save point
    std::string selectedText() const;

    sptr_t length() const { return execute(SCI_GETLENGTH); }
    sptr_t currentPos() const { return execute(SCI_GETCURRENTPOS); }
    sptr_t lineFromPos(sptr_t pos) const { return execute(SCI_LINEFROMPOSITION, static_cast<uptr_t>(pos)); }
    sptr_t lineCount() const { return execute(SCI_GETLINECOUNT); }
    bool isModified() const { return execute(SCI_GETMODIFY) != 0; }

    void gotoLine(sptr_t zeroBasedLine, sptr_t zeroBasedColumn = 0);
    // Upstream "Toggle Single Line Comment" (Ctrl+Q) using the language's commentLine.
    void toggleLineComment();
    void setEolMode(int scEolMode, bool convertExisting);
    // Line ending of the first line break in the text (SC_EOL_*), as upstream
    // does on load; `fallback` when the text has no line break.
    int detectEolMode(int fallback = SC_EOL_LF) const;

    // Bookmarks (upstream Search > Bookmark): marker in the symbol margin.
    static constexpr int kBookmarkMarker = 24; // below SC_MASK_FOLDERS (25..31)
    void toggleBookmark(sptr_t line);
    void toggleBookmarkAtCaret() { toggleBookmark(lineFromPos(currentPos())); }
    bool gotoNextBookmark(bool forward); // wraps around; false if none

    ViewPosition position() const;
    // Safe to call before the widget is mapped: scrolling is re-applied once
    // it has a size. Positions are clamped to the current text.
    void restorePosition(const ViewPosition& pos);

    std::vector<sptr_t> bookmarkLines() const;
    void setBookmarks(const std::vector<sptr_t>& lines);
    std::vector<sptr_t> contractedFoldLines() const;
    void contractFolds(const std::vector<sptr_t>& headerLines);

    // Upstream Preferences > Auto-Indent ("basic" mode), on by default.
    bool autoIndent = true;

    // Callbacks wired by the owning window.
    std::function<void()> onUpdateUI;                 // caret/selection/content moved
    std::function<void(bool modified)> onSavePoint;   // save point reached/left
    std::function<void(std::string_view uriList)> onUriDropped;
    std::function<void()> onTextModified;             // text inserted or deleted
    std::function<void()> onFocusIn;

private:
    static void onNotify(GtkWidget*, gint, gpointer notification, gpointer self);
    void handleNotify(const SCNotification& n);
    void setupDefaults();
    void applyGlobalStyles(const core::StyleCatalog& styles);
    void applyStyle(const core::Style& s);
    void updateLineNumberWidth();
    void highlightBraces();
    void maintainIndentation(int ch);

    GtkWidget* widget_ = nullptr;
    SciFnDirect directFn_ = nullptr;
    sptr_t directPtr_ = 0;
    const core::Language* lang_ = nullptr;
    int lineNumberDigits_ = 0;
    bool centreOnMap_ = false;
    sptr_t pendingFirstLine_ = -1;  // restorePosition() before the widget had a size
    sptr_t pendingXOffset_ = 0;
};

// Maps Windows-only font names in Notepad++ themes to a font that exists on
// this system (falls back to the fontconfig "Monospace" alias).
std::string resolveFontName(std::string_view requested);

} // namespace npp::ui
