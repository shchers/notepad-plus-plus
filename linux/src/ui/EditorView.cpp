#include "ui/EditorView.h"

#include "core/EditOps.h"
#include "core/LexerSetup.h"

#include <ILexer.h> // must precede Lexilla.h
#include <Lexilla.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace npp::ui {

namespace {

constexpr int kMarginLineNumber = 0;
constexpr int kMarginSymbol = 1;
constexpr int kMarginFold = 2;

constexpr sptr_t opaque(std::uint32_t bgr) { return static_cast<sptr_t>(bgr | 0xFF000000u); }

const std::set<std::string>& installedFontFamilies()
{
    static const std::set<std::string> families = [] {
        std::set<std::string> out;
        PangoFontMap* map = pango_cairo_font_map_get_default(); // owned by pango
        PangoFontFamily** fams = nullptr;
        int n = 0;
        pango_font_map_list_families(map, &fams, &n);
        for (int i = 0; i < n; ++i)
            out.insert(pango_font_family_get_name(fams[i]));
        g_free(fams);
        return out;
    }();
    return families;
}

} // namespace

std::string resolveFontName(std::string_view requested)
{
    if (requested.empty())
        return {};
    std::string name(requested);
    if (installedFontFamilies().count(name))
        return name;
    static const std::set<std::string> windowsMono = {
        "Courier New", "Consolas", "Lucida Console", "Courier", "Fixedsys", "Terminal", "Cascadia Code",
        "Cascadia Mono"};
    if (windowsMono.count(name))
        return "Monospace";
    return name; // let fontconfig substitute
}

EditorView::EditorView()
{
    widget_ = scintilla_new();
    g_object_ref_sink(widget_);
    auto* sci = SCINTILLA(widget_);
    directFn_ = reinterpret_cast<SciFnDirect>(scintilla_send_message(sci, SCI_GETDIRECTFUNCTION, 0, 0));
    directPtr_ = scintilla_send_message(sci, SCI_GETDIRECTPOINTER, 0, 0);
    g_signal_connect(widget_, SCINTILLA_NOTIFY, G_CALLBACK(&EditorView::onNotify), this);
    g_signal_connect_after(widget_, "size-allocate", G_CALLBACK(+[](GtkWidget*, GdkRectangle*, gpointer p) {
                               auto* self = static_cast<EditorView*>(p);
                               if (self->centreOnMap_) {
                                   self->centreOnMap_ = false;
                                   self->execute(SCI_VERTICALCENTRECARET);
                               }
                               if (self->pendingFirstLine_ >= 0) {
                                   self->execute(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(self->pendingFirstLine_));
                                   self->execute(SCI_SETXOFFSET, static_cast<uptr_t>(self->pendingXOffset_));
                                   self->pendingFirstLine_ = -1;
                               }
                           }), this);
    g_signal_connect(widget_, "focus-in-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
                         auto* self = static_cast<EditorView*>(p);
                         if (self->onFocusIn)
                             self->onFocusIn();
                         return FALSE;
                     }), this);
    setupDefaults();
}

EditorView::~EditorView()
{
    g_signal_handlers_disconnect_by_data(widget_, this);
    g_object_unref(widget_);
}

void EditorView::setupDefaults()
{
    execute(SCI_SETCODEPAGE, SC_CP_UTF8);
    execute(SCI_SETEOLMODE, SC_EOL_LF);
    execute(SCI_SETTABWIDTH, 4);
    execute(SCI_SETUSETABS, 1);
    execute(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);
    execute(SCI_SETSCROLLWIDTH, 1);
    execute(SCI_SETSCROLLWIDTHTRACKING, 1);
    execute(SCI_SETCARETLINEVISIBLEALWAYS, 1);
    setModEventMask(0);

    // Multi-editing and column mode, as in upstream defaults.
    execute(SCI_SETMULTIPLESELECTION, 1);
    execute(SCI_SETADDITIONALSELECTIONTYPING, 1);
    execute(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    execute(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);
    execute(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);

    // Margins: 0 line numbers, 1 bookmarks, 2 folding (upstream layout).
    execute(SCI_SETMARGINTYPEN, kMarginLineNumber, SC_MARGIN_NUMBER);
    execute(SCI_SETMARGINTYPEN, kMarginSymbol, SC_MARGIN_SYMBOL);
    execute(SCI_SETMARGINMASKN, kMarginSymbol, 1 << kBookmarkMarker);
    execute(SCI_SETMARGINWIDTHN, kMarginSymbol, 16);
    execute(SCI_SETMARGINSENSITIVEN, kMarginSymbol, 1);
    execute(SCI_MARKERDEFINE, kBookmarkMarker, SC_MARK_BOOKMARK);
    execute(SCI_MARKERSETFORE, kBookmarkMarker, 0x800000);
    execute(SCI_MARKERSETBACK, kBookmarkMarker, 0xFF8000);

    execute(SCI_SETMARGINTYPEN, kMarginFold, SC_MARGIN_SYMBOL);
    execute(SCI_SETMARGINMASKN, kMarginFold, SC_MASK_FOLDERS);
    execute(SCI_SETMARGINWIDTHN, kMarginFold, 14);
    execute(SCI_SETMARGINSENSITIVEN, kMarginFold, 1);
    execute(SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_SHOW | SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE);
    execute(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
    // "Box tree" fold markers — upstream default folder margin style.
    const std::pair<int, int> markers[] = {
        {SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS},        {SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS},
        {SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE},            {SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER},
        {SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED}, {SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED},
        {SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER}};
    for (auto [num, sym] : markers)
        execute(SCI_MARKERDEFINE, num, sym);
    execute(SCI_MARKERENABLEHIGHLIGHT, 1);

    updateLineNumberWidth();
}

void EditorView::setLineNumbersVisible(bool on)
{
    if (showLineNumbers == on)
        return;
    showLineNumbers = on;
    lineNumberDigits_ = -1;
    updateLineNumberWidth();
}

void EditorView::setBookmarkMarginVisible(bool on)
{
    execute(SCI_SETMARGINWIDTHN, kMarginSymbol, on ? 16 : 0);
}

void EditorView::setFoldStyle(int style)
{
    // Upstream ScintillaEditView::setMakerStyle / markersArray.
    static const int kMarkers[] = {SC_MARKNUM_FOLDEROPEN, SC_MARKNUM_FOLDER, SC_MARKNUM_FOLDERSUB, SC_MARKNUM_FOLDERTAIL,
                                   SC_MARKNUM_FOLDEREND, SC_MARKNUM_FOLDEROPENMID, SC_MARKNUM_FOLDERMIDTAIL};
    static const int kStyles[4][7] = {
        {SC_MARK_MINUS, SC_MARK_PLUS, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY},
        {SC_MARK_ARROWDOWN, SC_MARK_ARROW, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY, SC_MARK_EMPTY},
        {SC_MARK_CIRCLEMINUS, SC_MARK_CIRCLEPLUS, SC_MARK_VLINE, SC_MARK_LCORNERCURVE, SC_MARK_CIRCLEPLUSCONNECTED,
         SC_MARK_CIRCLEMINUSCONNECTED, SC_MARK_TCORNERCURVE},
        {SC_MARK_BOXMINUS, SC_MARK_BOXPLUS, SC_MARK_VLINE, SC_MARK_LCORNER, SC_MARK_BOXPLUSCONNECTED,
         SC_MARK_BOXMINUSCONNECTED, SC_MARK_TCORNER},
    };
    if (style < 0 || style > 3) {
        execute(SCI_SETMARGINWIDTHN, kMarginFold, 0);
        return;
    }
    execute(SCI_SETMARGINWIDTHN, kMarginFold, 14);
    for (int i = 0; i < 7; ++i)
        execute(SCI_MARKERDEFINE, static_cast<uptr_t>(kMarkers[i]), kStyles[style][i]);
}

void EditorView::applyStyle(const core::Style& s)
{
    if (s.id < 0 || s.id > STYLE_MAX)
        return;
    const uptr_t id = static_cast<uptr_t>(s.id);
    if (!s.fontName.empty()) {
        const std::string font = resolveFontName(s.fontName);
        execute(SCI_STYLESETFONT, id, reinterpret_cast<sptr_t>(font.c_str()));
    }
    if (s.fontSize && *s.fontSize > 0)
        execute(SCI_STYLESETSIZE, id, *s.fontSize);
    if (s.fontStyle) {
        execute(SCI_STYLESETBOLD, id, (*s.fontStyle & core::kBold) != 0);
        execute(SCI_STYLESETITALIC, id, (*s.fontStyle & core::kItalic) != 0);
        execute(SCI_STYLESETUNDERLINE, id, (*s.fontStyle & core::kUnderline) != 0);
    }
    if (s.fg)
        execute(SCI_STYLESETFORE, id, *s.fg);
    if (s.bg)
        execute(SCI_STYLESETBACK, id, *s.bg);
}

void EditorView::applyGlobalStyles(const core::StyleCatalog& styles)
{
    if (const core::Style* def = styles.globalById(STYLE_DEFAULT))
        applyStyle(*def);
    execute(SCI_STYLECLEARALL); // propagate default style to all styles

    // Real Scintilla styles among the globals (32..39: line numbers, braces,
    // indent guides...). IDs < 32 there are upstream indicator IDs, not styles.
    for (const core::Style& g : styles.globals())
        if (g.id > STYLE_DEFAULT && g.id <= STYLE_LASTPREDEFINED)
            applyStyle(g);

    auto byName = [&](const char* name) { return styles.global(name); };
    if (auto* s = byName("Current line background colour"); s && s->bg)
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, opaque(*s->bg));
    if (auto* s = byName("Selected text colour"); s && s->bg) {
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, opaque(*s->bg));
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, opaque(*s->bg));
    }
    if (auto* s = byName("Multi-selected text color"); s && s->bg)
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_ADDITIONAL_BACK, opaque(*s->bg));
    if (auto* s = byName("Caret colour"); s && s->fg)
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, opaque(*s->fg));
    if (auto* s = byName("Multi-edit carets color"); s && s->fg)
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_ADDITIONAL, opaque(*s->fg));
    if (auto* s = byName("White space symbol"); s && s->fg)
        execute(SCI_SETELEMENTCOLOUR, SC_ELEMENT_WHITE_SPACE, opaque(*s->fg));
    if (auto* s = byName("Bookmark margin"); s && s->bg) {
        execute(SCI_SETMARGINTYPEN, kMarginSymbol, SC_MARGIN_COLOUR);
        execute(SCI_SETMARGINBACKN, kMarginSymbol, *s->bg);
    }
    if (auto* s = byName("Fold margin")) {
        if (s->bg)
            execute(SCI_SETFOLDMARGINCOLOUR, 1, *s->bg);
        if (s->fg)
            execute(SCI_SETFOLDMARGINHICOLOUR, 1, *s->fg);
    }
    // Same colour mapping as upstream ScintillaEditView::defineMarker.
    const auto* fold = byName("Fold");
    const auto* foldActive = byName("Fold active");
    for (int m = SC_MARKNUM_FOLDEREND; m <= SC_MARKNUM_FOLDEROPEN; ++m) {
        if (fold && fold->bg)
            execute(SCI_MARKERSETFORE, m, *fold->bg);
        if (fold && fold->fg)
            execute(SCI_MARKERSETBACK, m, *fold->fg);
        if (foldActive && foldActive->fg)
            execute(SCI_MARKERSETBACKSELECTED, m, *foldActive->fg);
    }
    // Search indicators: Find Mark Style, Mark Style 1-5, Incremental highlight
    // all, Smart Highlighting; their styleID is the indicator number, drawn as
    // upstream ScintillaEditView::init does.
    for (int ind : {21, 22, 23, 24, 25, 28, 29, 31}) {
        execute(SCI_INDICSETSTYLE, static_cast<uptr_t>(ind), INDIC_ROUNDBOX);
        execute(SCI_INDICSETALPHA, static_cast<uptr_t>(ind), 100);
        execute(SCI_INDICSETOUTLINEALPHA, static_cast<uptr_t>(ind), 150);
        execute(SCI_INDICSETUNDER, static_cast<uptr_t>(ind), true);
        if (const core::Style* s = styles.globalById(ind); s && s->bg)
            execute(SCI_INDICSETFORE, static_cast<uptr_t>(ind), *s->bg);
    }
    lineNumberDigits_ = 0; // font may have changed
    updateLineNumberWidth();
}

void EditorView::applyLanguage(const core::Language& lang, const core::LanguageCatalog& langs, const core::StyleCatalog& styles)
{
    lang_ = &lang;
    applyGlobalStyles(styles);

    ILexer5* lexer = lang.isPlainText() ? nullptr : CreateLexer(lang.lexer.c_str());
    execute(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(lexer));
    if (!lexer) {
        execute(SCI_COLOURISE, 0, -1);
        return;
    }

    const std::pair<const char*, const char*> props[] = {
        {"fold", "1"}, {"fold.comment", "1"}, {"fold.preprocessor", "1"}, {"fold.html", "1"}, {"fold.compact", "0"},
        {"lexer.cpp.backquoted.strings", lang.name == "go" ? "1" : "0"}, // as upstream setCppLexer for Go
        {"fold.quotes.python", "1"}, {"lexer.python.decorator.attributes", "1"},
    };
    for (auto [key, val] : props)
        execute(SCI_SETPROPERTY, reinterpret_cast<uptr_t>(key), reinterpret_cast<sptr_t>(val));

    const core::LexerSetup plan = core::planLexerSetup(lang, langs, styles);
    for (int i = 0; i < core::kKeywordSets; ++i)
        execute(SCI_SETKEYWORDS, static_cast<uptr_t>(i), reinterpret_cast<sptr_t>(plan.keywordSets[i].c_str()));
    for (const std::string& group : plan.styleGroups)
        if (const auto* st = styles.forLanguage(group))
            for (const core::Style& s : *st)
                applyStyle(s);

    execute(SCI_COLOURISE, 0, -1);
}

void EditorView::applyLexer(const char* lexillaName, std::string_view styleGroup, const core::StyleCatalog& styles)
{
    lang_ = nullptr;
    applyGlobalStyles(styles);
    execute(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(CreateLexer(lexillaName)));
    execute(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"), reinterpret_cast<sptr_t>("1"));
    if (const auto* st = styles.forLanguage(styleGroup))
        for (const core::Style& s : *st)
            applyStyle(s);
    execute(SCI_COLOURISE, 0, -1);
}

void EditorView::applyUserLanguage(const core::UserLanguage& udl, const core::StyleCatalog& styles, long udlId)
{
    lang_ = nullptr;
    applyGlobalStyles(styles);
    execute(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(CreateLexer("user")));
    // The document identifies LexUser's per-buffer nesting state.
    const long bufferId = static_cast<long>(reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(execute(SCI_GETDOCPOINTER))) & 0x7FFFFFFF);
    const core::UserLexerSetup plan = core::planUserLexer(udl, udlId, bufferId);
    for (const auto& [k, v] : plan.properties)
        execute(SCI_SETPROPERTY, reinterpret_cast<uptr_t>(k.c_str()), reinterpret_cast<sptr_t>(v.c_str()));
    for (std::size_t i = 0; i < plan.keywordSets.size(); ++i)
        execute(SCI_SETKEYWORDS, i, reinterpret_cast<sptr_t>(plan.keywordSets[i].c_str()));
    for (const core::Style& s : plan.styles)
        applyStyle(s);
    execute(SCI_COLOURISE, 0, -1);
}

std::string EditorView::text() const
{
    const sptr_t len = length();
    const char* p = reinterpret_cast<const char*>(execute(SCI_GETCHARACTERPOINTER));
    return p ? std::string(p, static_cast<std::size_t>(len)) : std::string();
}

void EditorView::setText(std::string_view utf8)
{
    execute(SCI_SETUNDOCOLLECTION, 0);
    execute(SCI_CLEARALL);
    execute(SCI_ADDTEXT, utf8.size(), reinterpret_cast<sptr_t>(utf8.data()));
    execute(SCI_SETUNDOCOLLECTION, 1);
    execute(SCI_EMPTYUNDOBUFFER);
    execute(SCI_SETSAVEPOINT);
    execute(SCI_GOTOPOS, 0);
    updateLineNumberWidth();
}

std::string EditorView::selectedText() const
{
    const sptr_t a = execute(SCI_GETSELECTIONSTART), b = execute(SCI_GETSELECTIONEND);
    if (b <= a)
        return {};
    const char* p = reinterpret_cast<const char*>(execute(SCI_GETRANGEPOINTER, static_cast<uptr_t>(a), b - a));
    return p ? std::string(p, static_cast<std::size_t>(b - a)) : std::string();
}

void EditorView::gotoLine(sptr_t line, sptr_t column)
{
    line = std::clamp<sptr_t>(line, 0, lineCount() - 1);
    execute(SCI_ENSUREVISIBLEENFORCEPOLICY, static_cast<uptr_t>(line));
    const sptr_t pos = execute(SCI_FINDCOLUMN, static_cast<uptr_t>(line), std::max<sptr_t>(column, 0));
    execute(SCI_GOTOPOS, static_cast<uptr_t>(pos));
    if (gtk_widget_get_mapped(widget_)) {
        execute(SCI_VERTICALCENTRECARET);
        return;
    }
    // Not on screen yet (e.g. -n<line> at startup): Scintilla has no height to
    // centre in, so centre once the widget has been mapped and sized.
    centreOnMap_ = true;
}

void EditorView::toggleLineComment()
{
    if (lang_)
        core::lineComment(sci(), lang_->commentLine, core::LineComment::Toggle);
}

void EditorView::setEolMode(int mode, bool convertExisting)
{
    execute(SCI_SETEOLMODE, static_cast<uptr_t>(mode));
    if (convertExisting)
        execute(SCI_CONVERTEOLS, static_cast<uptr_t>(mode));
}

int EditorView::detectEolMode(int fallback) const
{
    const sptr_t len = length();
    const char* p = reinterpret_cast<const char*>(execute(SCI_GETCHARACTERPOINTER));
    for (sptr_t i = 0; p && i < len; ++i) {
        if (p[i] == '\n')
            return SC_EOL_LF;
        if (p[i] == '\r')
            return (i + 1 < len && p[i + 1] == '\n') ? SC_EOL_CRLF : SC_EOL_CR;
    }
    return fallback;
}

void EditorView::toggleBookmark(sptr_t line)
{
    const sptr_t mask = execute(SCI_MARKERGET, static_cast<uptr_t>(line));
    if (mask & (1 << kBookmarkMarker))
        execute(SCI_MARKERDELETE, static_cast<uptr_t>(line), kBookmarkMarker);
    else
        execute(SCI_MARKERADD, static_cast<uptr_t>(line), kBookmarkMarker);
}

bool EditorView::gotoNextBookmark(bool forward)
{
    const sptr_t mask = 1 << kBookmarkMarker;
    const sptr_t cur = lineFromPos(currentPos());
    sptr_t found = forward ? execute(SCI_MARKERNEXT, static_cast<uptr_t>(cur + 1), mask)
                           : execute(SCI_MARKERPREVIOUS, static_cast<uptr_t>(cur - 1), mask);
    if (found < 0) // wrap around like upstream
        found = forward ? execute(SCI_MARKERNEXT, 0, mask)
                        : execute(SCI_MARKERPREVIOUS, static_cast<uptr_t>(lineCount() - 1), mask);
    if (found < 0)
        return false;
    gotoLine(found);
    return true;
}

ViewPosition EditorView::position() const
{
    ViewPosition p;
    p.firstVisibleLine = pendingFirstLine_ >= 0 ? pendingFirstLine_ : execute(SCI_GETFIRSTVISIBLELINE);
    p.xOffset = pendingFirstLine_ >= 0 ? pendingXOffset_ : execute(SCI_GETXOFFSET);
    p.selMode = execute(SCI_GETSELECTIONMODE);
    if (p.selMode == SC_SEL_RECTANGLE || p.selMode == SC_SEL_THIN) {
        p.startPos = execute(SCI_GETRECTANGULARSELECTIONANCHOR);
        p.endPos = execute(SCI_GETRECTANGULARSELECTIONCARET);
    } else {
        p.startPos = execute(SCI_GETANCHOR);
        p.endPos = execute(SCI_GETCURRENTPOS);
    }
    p.scrollWidth = execute(SCI_GETSCROLLWIDTH);
    return p;
}

void EditorView::restorePosition(const ViewPosition& pos)
{
    // Same sequence as upstream ScintillaEditView::restoreCurrentPosPreStep.
    const sptr_t len = length();
    const uptr_t anchor = static_cast<uptr_t>(std::clamp<sptr_t>(pos.startPos, 0, len));
    const sptr_t caret = std::clamp<sptr_t>(pos.endPos, 0, len);
    execute(SCI_SETSELECTIONMODE, static_cast<uptr_t>(pos.selMode));
    if (pos.selMode == SC_SEL_RECTANGLE || pos.selMode == SC_SEL_THIN) {
        execute(SCI_SETRECTANGULARSELECTIONANCHOR, anchor);
        execute(SCI_SETRECTANGULARSELECTIONCARET, static_cast<uptr_t>(caret));
    } else {
        execute(SCI_SETANCHOR, anchor);
        execute(SCI_SETCURRENTPOS, static_cast<uptr_t>(caret));
    }
    execute(SCI_CANCEL); // stop extending the selection in the restored mode
    if (pos.scrollWidth > 0)
        execute(SCI_SETSCROLLWIDTH, static_cast<uptr_t>(pos.scrollWidth));
    const sptr_t first = std::max<sptr_t>(pos.firstVisibleLine, 0);
    execute(SCI_SETFIRSTVISIBLELINE, static_cast<uptr_t>(first));
    execute(SCI_SETXOFFSET, static_cast<uptr_t>(std::max<sptr_t>(pos.xOffset, 0)));
    if (!gtk_widget_get_mapped(widget_)) {
        // Without a height Scintilla clamps the top line; re-apply once sized.
        pendingFirstLine_ = first;
        pendingXOffset_ = std::max<sptr_t>(pos.xOffset, 0);
    }
}

std::vector<sptr_t> EditorView::bookmarkLines() const
{
    std::vector<sptr_t> lines;
    for (sptr_t l = execute(SCI_MARKERNEXT, 0, 1 << kBookmarkMarker); l >= 0;
         l = execute(SCI_MARKERNEXT, static_cast<uptr_t>(l + 1), 1 << kBookmarkMarker))
        lines.push_back(l);
    return lines;
}

void EditorView::setBookmarks(const std::vector<sptr_t>& lines)
{
    execute(SCI_MARKERDELETEALL, kBookmarkMarker);
    for (const sptr_t l : lines)
        if (l >= 0 && l < lineCount())
            execute(SCI_MARKERADD, static_cast<uptr_t>(l), kBookmarkMarker);
}

std::vector<sptr_t> EditorView::contractedFoldLines() const
{
    std::vector<sptr_t> lines;
    for (sptr_t l = execute(SCI_CONTRACTEDFOLDNEXT, 0); l >= 0; l = execute(SCI_CONTRACTEDFOLDNEXT, static_cast<uptr_t>(l + 1)))
        lines.push_back(l);
    return lines;
}

void EditorView::contractFolds(const std::vector<sptr_t>& headerLines)
{
    if (headerLines.empty())
        return;
    execute(SCI_COLOURISE, 0, -1); // fold levels come from the lexer
    for (const sptr_t l : headerLines)
        if (l >= 0 && l < lineCount() && (execute(SCI_GETFOLDLEVEL, static_cast<uptr_t>(l)) & SC_FOLDLEVELHEADERFLAG))
            execute(SCI_FOLDLINE, static_cast<uptr_t>(l), SC_FOLDACTION_CONTRACT);
}

void EditorView::maintainIndentation(int ch)
{
    // Upstream "basic" auto-indent: a new line inherits the previous line's
    // indentation. With CR LF the '\n' arrives second, so act on that one.
    const int eol = static_cast<int>(execute(SCI_GETEOLMODE));
    if ((eol == SC_EOL_CRLF && ch != '\n') || (eol == SC_EOL_CR && ch != '\r') || (eol == SC_EOL_LF && ch != '\n'))
        return;
    const sptr_t line = lineFromPos(currentPos());
    if (line == 0)
        return;
    const sptr_t indent = execute(SCI_GETLINEINDENTATION, static_cast<uptr_t>(line - 1));
    if (indent <= 0)
        return;
    execute(SCI_SETLINEINDENTATION, static_cast<uptr_t>(line), indent);
    execute(SCI_GOTOPOS, static_cast<uptr_t>(execute(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(line))));
}

void EditorView::updateLineNumberWidth()
{
    int digits = 1;
    for (sptr_t n = lineCount(); n >= 10; n /= 10)
        ++digits;
    digits = std::max(digits, 3);
    if (!showLineNumbers) {
        if (lineNumberDigits_ != 0)
            execute(SCI_SETMARGINWIDTHN, kMarginLineNumber, 0);
        lineNumberDigits_ = 0;
        return;
    }
    if (digits == lineNumberDigits_)
        return;
    lineNumberDigits_ = digits;
    const std::string sample = "_" + std::string(static_cast<std::size_t>(digits), '9');
    const sptr_t w = execute(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>(sample.c_str()));
    execute(SCI_SETMARGINWIDTHN, kMarginLineNumber, w + 4);
}

void EditorView::highlightBraces()
{
    auto isBrace = [](int c) { return c && std::string_view("()[]{}").find(static_cast<char>(c)) != std::string_view::npos; };
    const sptr_t pos = currentPos();
    sptr_t brace = -1;
    if (pos > 0 && isBrace(static_cast<int>(execute(SCI_GETCHARAT, static_cast<uptr_t>(pos - 1)))))
        brace = pos - 1;
    else if (pos < length() && isBrace(static_cast<int>(execute(SCI_GETCHARAT, static_cast<uptr_t>(pos)))))
        brace = pos;
    if (brace < 0) {
        execute(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
        return;
    }
    const sptr_t match = execute(SCI_BRACEMATCH, static_cast<uptr_t>(brace), 0);
    if (match < 0)
        execute(SCI_BRACEBADLIGHT, static_cast<uptr_t>(brace));
    else
        execute(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(brace), match);
}

void EditorView::onNotify(GtkWidget*, gint, gpointer notification, gpointer self)
{
    static_cast<EditorView*>(self)->handleNotify(*static_cast<const SCNotification*>(notification));
}

void EditorView::setModEventMask(int extra)
{
    // Upstream MODEVENTMASK_ON: what plugins get by default.
    execute(SCI_SETMODEVENTMASK, static_cast<uptr_t>(SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_PERFORMED_UNDO | SC_PERFORMED_REDO |
                                                     SC_MOD_CHANGEINDICATOR | extra));
}

void EditorView::handleNotify(const SCNotification& n)
{
    if (onScintillaNotify)
        onScintillaNotify(n);
    switch (n.nmhdr.code) {
    case SCN_UPDATEUI:
        highlightBraces();
        if (onUpdateUI)
            onUpdateUI();
        break;
    case SCN_SAVEPOINTREACHED:
    case SCN_SAVEPOINTLEFT:
        if (onSavePoint)
            onSavePoint(n.nmhdr.code == SCN_SAVEPOINTLEFT);
        break;
    case SCN_MODIFIED:
        if (n.linesAdded != 0)
            updateLineNumberWidth();
        if ((n.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) && onTextModified)
            onTextModified();
        break;
    case SCN_CHARADDED:
        if (autoIndent && (n.ch == '\n' || n.ch == '\r'))
            maintainIndentation(n.ch);
        break;
    case SCN_MARGINCLICK:
        if (n.margin == kMarginSymbol)
            toggleBookmark(lineFromPos(n.position));
        break;
    case SCN_DOUBLECLICK:
        if (onDoubleClick)
            onDoubleClick(n.position);
        break;
    case SCN_MACRORECORD:
        if (onMacroRecord)
            onMacroRecord(static_cast<int>(n.message), n.wParam, n.lParam);
        break;
    case SCN_URIDROPPED:
        if (onUriDropped && n.text)
            onUriDropped(n.text);
        break;
    default:
        break;
    }
}

} // namespace npp::ui
