#include "ui/EditorView.h"

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
    execute(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);

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
    if (!lang_ || lang_->commentLine.empty())
        return;
    const std::string& tok = lang_->commentLine;
    const sptr_t selStart = execute(SCI_GETSELECTIONSTART), selEnd = execute(SCI_GETSELECTIONEND);
    const sptr_t first = lineFromPos(selStart);
    sptr_t last = lineFromPos(selEnd);
    if (last > first && execute(SCI_POSITIONFROMLINE, static_cast<uptr_t>(last)) == selEnd)
        --last; // selection ends at column 0: that line is not part of it

    auto indentPos = [&](sptr_t l) { return execute(SCI_GETLINEINDENTPOSITION, static_cast<uptr_t>(l)); };
    auto lineEnd = [&](sptr_t l) { return execute(SCI_GETLINEENDPOSITION, static_cast<uptr_t>(l)); };
    auto startsWithTok = [&](sptr_t l) {
        const sptr_t p = indentPos(l);
        if (lineEnd(l) - p < static_cast<sptr_t>(tok.size()))
            return false;
        const char* s = reinterpret_cast<const char*>(execute(SCI_GETRANGEPOINTER, static_cast<uptr_t>(p), tok.size()));
        return s && std::string_view(s, tok.size()) == tok;
    };

    bool allCommented = true;
    for (sptr_t l = first; l <= last; ++l)
        if (indentPos(l) != lineEnd(l) && !startsWithTok(l))
            allCommented = false;

    execute(SCI_BEGINUNDOACTION);
    for (sptr_t l = first; l <= last; ++l) {
        const sptr_t p = indentPos(l);
        if (p == lineEnd(l))
            continue; // blank line
        if (allCommented) {
            sptr_t n = static_cast<sptr_t>(tok.size());
            if (p + n < lineEnd(l) && execute(SCI_GETCHARAT, static_cast<uptr_t>(p + n)) == ' ')
                ++n;
            execute(SCI_DELETERANGE, static_cast<uptr_t>(p), n);
        } else {
            const std::string ins = tok + " ";
            execute(SCI_INSERTTEXT, static_cast<uptr_t>(p), reinterpret_cast<sptr_t>(ins.c_str()));
        }
    }
    execute(SCI_ENDUNDOACTION);
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

void EditorView::handleNotify(const SCNotification& n)
{
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
        break;
    case SCN_CHARADDED:
        if (autoIndent && (n.ch == '\n' || n.ch == '\r'))
            maintainIndentation(n.ch);
        break;
    case SCN_MARGINCLICK:
        if (n.margin == kMarginSymbol)
            toggleBookmark(lineFromPos(n.position));
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
