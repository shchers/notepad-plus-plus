// GTK counterpart of upstream ScintillaEditView.
//
// The important seam: execute(msg, w, l) has the same shape and the same
// implementation strategy (Scintilla direct function pointer) as upstream's
// ScintillaEditView::execute(). Upstream code calls execute(SCI_...) ~1900
// times; keeping this signature lets that code move over with type changes
// only (WPARAM/LPARAM -> uptr_t/sptr_t), not rewrites.
#pragma once

#include "core/EditorFeatures.h"
#include "core/LanguageCatalog.h"
#include "core/Sci.h"
#include "core/StyleCatalog.h"
#include "core/UserLanguage.h"

// Order matters: ScintillaWidget.h expects GTK types to be declared.
#include <gtk/gtk.h>
#include <Scintilla.h>
#include <ScintillaWidget.h>

#include <cstdint>
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

// What the editing aids do (Preferences); one instance shared by the views.
struct EditorOptions {
    int autoIndentMode = 2;            // 0 none, 1 basic, 2 advanced
    bool braceMatch = true;
    bool smartHighlight = true;
    bool smartWholeWord = true;
    bool smartMatchCase = false;
    bool smartOtherView = true;
    bool tagMatch = true;
    bool tagAttributes = true;
    bool tagZones = true;
    bool links = true;
    std::string linkSchemes;
    char delimOpen = '(', delimClose = ')';
    bool delimMultiLine = false;
    bool preventC0 = true;
    bool keepSelOnRightClick = true;
    bool lineNumberDynamic = true;
    core::MatchedPairs pairs;
    core::AutoCompleteOptions autoComplete;
    bool acInsertTab = true, acInsertEnter = true;
    // Large files (Preferences > Performance), when restricted.
    bool largeBraceMatch = false, largeAutoComplete = false, largeSmartHighlight = false, largeLinks = false;
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
    // The same seam for UI-free core code (search, line operations, macros).
    core::Sci sci() const { return {directFn_, directPtr_}; }

    // Applies global styles (default font, caret line, selection, margins)
    // and then the language's lexer, keywords and styles. `langs` is needed
    // because some lexers pull keywords from other languages (embedded JS in
    // HTML, Doxygen keywords from C++).
    void applyLanguage(const core::Language& lang, const core::LanguageCatalog& langs, const core::StyleCatalog& styles);
    const core::Language* language() const { return lang_; }
    // A Lexilla lexer by name with the theme's styles for `styleGroup`
    // (stylers.xml LexerType name) — for views that are not documents, such
    // as the search results panel.
    void applyLexer(const char* lexillaName, std::string_view styleGroup, const core::StyleCatalog& styles);
    // A User Defined Language through Lexilla's "user" lexer (upstream setUserLexer).
    void applyUserLanguage(const core::UserLanguage& udl, const core::StyleCatalog& styles, long udlId);

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
    static constexpr int kBookmarkMarker = 20; // upstream MARK_BOOKMARK, below the change history markers (21..24)
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

    // Upstream Preferences > Auto-Indent ("basic" mode), on by default
    // (used when no EditorOptions are set).
    bool autoIndent = true;
    // Editing aids; null = only brace matching and basic auto-indent.
    void setOptions(const EditorOptions* o) { opts_ = o; }
    // The language's auto-completion file (null = none).
    void setApi(const core::ApiFile* api) { api_ = api; }
    // Opened under the large file restriction (Preferences > Performance).
    bool largeFile = false;
    core::AutoCompleter& completer() { return completer_; }
    // Re-runs smart highlighting, tag matching and link marking.
    void refreshDecorations();
    // Upstream Change History (Preferences > Margins/Border/Edge); applies to
    // a document from its next load, as Scintilla needs an empty undo history.
    void setChangeHistory(bool margin, bool text);
    // View > Show Symbol's invisible characters and EOL appearance (Preferences > Editing 2).
    struct Representations {
        bool showNpc = false, showCcUniEol = false;
        bool npcCodepoint = false, npcCustomColor = false, includeCcUniEol = false;
        bool eolPlain = false, eolCustomColor = false;
        std::uint32_t npcColor = 0x6AB5FF, eolColor = 0xDADADA; // BGR
    };
    void applyRepresentations(const Representations& r);
    void refreshLineNumberWidth();
    // Clickable links: underline or not, a box under the mouse or a dotted line.
    void setLinkStyle(bool noUnderline, bool fullBox);
    std::function<EditorView*()> otherView;           // for smart highlighting
    std::function<void(const std::string& url)> onLinkClicked;

    // Callbacks wired by the owning window.
    std::function<void()> onUpdateUI;                 // caret/selection/content moved
    std::function<void(bool modified)> onSavePoint;   // save point reached/left
    std::function<void(std::string_view uriList)> onUriDropped;
    std::function<void()> onTextModified;             // text inserted or deleted
    std::function<void()> onFocusIn;
    std::function<void(sptr_t position)> onDoubleClick;
    // Every Scintilla notification, before the view handles it (plugins).
    std::function<void(const SCNotification&)> onScintillaNotify;
    // SCN_MACRORECORD while recording (SCI_STARTRECORD).
    std::function<void(int message, uptr_t wParam, sptr_t lParam)> onMacroRecord;
    bool showLineNumbers = true;
    void setLineNumbersVisible(bool on);
    // Upstream Preferences > Margins "Fold Margin Style": 0 Simple, 1 Arrow,
    // 2 Circle tree, 3 Box tree, 4 None.
    void setFoldStyle(int style);
    void setBookmarkMarginVisible(bool on);
    // SCN_MODIFIED events: upstream's MODEVENTMASK_ON plus `extra`
    // (NPPM_ADDSCNMODIFIEDFLAGS).
    void setModEventMask(int extra);

private:
    static void onNotify(GtkWidget*, gint, gpointer notification, gpointer self);
    void handleNotify(const SCNotification& n);
    void setupDefaults();
    void applyGlobalStyles(const core::StyleCatalog& styles);
    void applyStyle(const core::Style& s);
    void updateLineNumberWidth();
    void highlightBraces();
    void maintainIndentation(int ch);
    void onCharAdded(int ch);
    core::IndentStyle indentStyle() const;
    int markup() const;                 // 1 HTML, 2 XML, 0 other
    bool tagLanguage() const;

    GtkWidget* widget_ = nullptr;
    SciFnDirect directFn_ = nullptr;
    sptr_t directPtr_ = 0;
    const core::Language* lang_ = nullptr;
    int lineNumberDigits_ = 0;
    bool centreOnMap_ = false;
    sptr_t pendingFirstLine_ = -1;  // restorePosition() before the widget had a size
    sptr_t pendingXOffset_ = 0;
    const EditorOptions* opts_ = nullptr;
    const core::ApiFile* api_ = nullptr;
    core::AutoInserter inserter_;
    core::AutoCompleter completer_;
    int changeHistory_ = 0;             // SC_CHANGE_HISTORY_* flags
    int savedSelections_ = 0;           // right-click: selection kept
    std::vector<std::pair<sptr_t, sptr_t>> keptSelection_;
    guint keepSelIdle_ = 0;
};

// Maps Windows-only font names in Notepad++ themes to a font that exists on
// this system (falls back to the fontconfig "Monospace" alias).
std::string resolveFontName(std::string_view requested);

} // namespace npp::ui
