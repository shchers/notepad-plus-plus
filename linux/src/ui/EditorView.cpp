#include "ui/EditorView.h"

#include "core/EditOps.h"
#include "core/LexerSetup.h"

#include <ILexer.h> // must precede Lexilla.h
#include <Lexilla.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <vector>

namespace npp::ui {

namespace {

// Upstream ScintillaEditView.cpp: how View > Show Symbol shows invisible
// characters ({UTF-8, abbreviation, codepoint}).
constexpr char kZwsp[] = "\xE2\x80\x8B";
constexpr std::array<std::array<const char*, 3>, 64> g_ccUniEolChars{ {
    // C0
    {"\x00", "NUL", "U+0000"},               // U+0000 : Null
    {"\x01", "SOH", "U+0001"},               // U+0001 : Start of Heading
    {"\x02", "STX", "U+0002"},               // U+0002 : Start of Text
    {"\x03", "ETX", "U+0003"},               // U+0003 : End of Text
    {"\x04", "EOT", "U+0004"},               // U+0004 : End of Transmission
    {"\x05", "ENQ", "U+0005"},               // U+0005 : Enquiry
    {"\x06", "ACK", "U+0006"},               // U+0006 : Acknowledge
    {"\a", "BEL", "U+0007"},                 // U+0007 : Bell
    {"\b", "BS", "U+0008"},                  // U+0008 : Backspace
    {"\v", "VT", "U+000B"},                  // U+000B : Line Tabulation
    {"\f", "FF", "U+000C"},                  // U+000C : Form Feed
    {"\x0E", "SO", "U+000E"},                // U+000E : Shift Out
    {"\x0F", "SI", "U+000F"},                // U+000F : Shift In
    {"\x10", "DLE", "U+0010"},               // U+0010 : Data Link Escape
    {"\x11", "DC1", "U+0011"},               // U+0011 : Device Control One
    {"\x12", "DC2", "U+0012"},               // U+0012 : Device Control Two
    {"\x13", "DC3", "U+0013"},               // U+0013 : Device Control Three
    {"\x14", "DC4", "U+0014"},               // U+0014 : Device Control Four
    {"\x15", "NAK", "U+0015"},               // U+0015 : Negative Acknowledge
    {"\x16", "SYN", "U+0016"},               // U+0016 : Synchronous Idle
    {"\x17", "ETB", "U+0017"},               // U+0017 : End of Transmission Block
    {"\x18", "CAN", "U+0018"},               // U+0018 : Cancel
    {"\x19", "EM", "U+0019"},                // U+0019 : End of Medium
    {"\x1A", "SUB", "U+001A"},               // U+001A : Substitute
    {"\x1B", "ESC", "U+001B"},               // U+001B : Escape
    {"\x1C", "FS", "U+001C"},                // U+001C : Information Separator Four
    {"\x1D", "GS", "U+001D"},                // U+001D : Information Separator Three
    {"\x1E", "RS", "U+001E"},                // U+001E : Information Separator Two
    {"\x1F", "US", "U+001F"},                // U+001F : Information Separator One
    {"\x7F", "DEL", "U+007F"},               // U+007F : Delete
    // C1
    {"\xC2\x80", "PAD", "U+0080"},           // U+0080 : Padding Character
    {"\xC2\x81", "HOP", "U+0081"},           // U+0081 : High Octet Preset
    {"\xC2\x82", "BPH", "U+0082"},           // U+0082 : Break Permitted Here
    {"\xC2\x83", "NBH", "U+0083"},           // U+0083 : No Break Here
    {"\xC2\x84", "IND", "U+0084"},           // U+0084 : Index
    //{"\xC2\x85", "NEL", "U+0085"},          // U+0085 : Next Line
    {"\xC2\x86", "SSA", "U+0086"},           // U+0086 : Start of Selected Area
    {"\xC2\x87", "ESA", "U+0087"},           // U+0087 : End of Selected Area
    {"\xC2\x88", "HTS", "U+0088"},           // U+0088 : Character (Horizontal) Tabulation Set
    {"\xC2\x89", "HTJ", "U+0089"},           // U+0089 : Character (Horizontal) Tabulation With Justification
    {"\xC2\x8A", "VTS", "U+008A"},           // U+008A : Vertical (Line) Tabulation Set
    {"\xC2\x8B", "PLD", "U+008B"},           // U+008B : Partial Line Forward (Down)
    {"\xC2\x8C", "PLU", "U+008C"},           // U+008C : Partial Line Backward (Up)
    {"\xC2\x8D", "RI", "U+008D"},            // U+008D : Reverse Line Feed (Index)
    {"\xC2\x8E", "SS2", "U+008E"},           // U+008E : Single-Shift Two
    {"\xC2\x8F", "SS3", "U+008F"},           // U+008F : Single-Shift Three
    {"\xC2\x90", "DCS", "U+0090"},           // U+0090 : Device Control String
    {"\xC2\x91", "PU1", "U+0091"},           // U+0091 : Private Use One
    {"\xC2\x92", "PU2", "U+0092"},           // U+0092 : Private Use Two
    {"\xC2\x93", "STS", "U+0093"},           // U+0093 : Set Transmit State
    {"\xC2\x94", "CCH", "U+0094"},           // U+0094 : Cancel Character
    {"\xC2\x95", "MW", "U+0095"},            // U+0095 : Message Waiting
    {"\xC2\x96", "SPA", "U+0096"},           // U+0096 : Start of Protected Area
    {"\xC2\x97", "EPA", "U+0097"},           // U+0097 : End of Protected Area
    {"\xC2\x98", "SOS", "U+0098"},           // U+0098 : Start of String
    {"\xC2\x99", "SGCI", "U+0099"},          // U+0099 : Single Graphic Character Introducer
    {"\xC2\x9A", "SCI", "U+009A"},           // U+009A : Single Character Introducer
    {"\xC2\x9B", "CSI", "U+009B"},           // U+009B : Control Sequence Introducer
    {"\xC2\x9C", "ST", "U+009C"},            // U+009C : String Terminator
    {"\xC2\x9D", "OSC", "U+009D"},           // U+009D : Operating System Command
    {"\xC2\x9E", "PM", "U+009E"},            // U+009E : Private Message
    {"\xC2\x9F", "APC", "U+009F"},           // U+009F : Application Program Command
    // Unicode EOL
    {"\xC2\x85", "NEL", "U+0085"},           // U+0085 : Next Line
    {"\xE2\x80\xA8", "LS", "U+2028"},        // U+2028 : Line Separator
    {"\xE2\x80\xA9", "PS", "U+2029"}         // U+2029 : Paragraph Separator
} };
constexpr std::array<std::array<const char*, 3>, 49> g_nonPrintingChars{ {
    {"\xC2\xA0", "NBSP", "U+00A0"},          // U+00A0 : no-break space
    {"\xC2\xAD", "SHY", "U+00AD"},           // U+00AD : soft hyphen
    {"\xD8\x9C", "ALM", "U+061C"},           // U+061C : arabic letter mark
    {"\xDC\x8F", "SAM", "U+070F"},           // U+070F : syriac abbreviation mark
    {"\xE1\x9A\x80", "OSPM", "U+1680"},      // U+1680 : ogham space mark
    {"\xE1\xA0\x8E", "MVS", "U+180E"},       // U+180E : mongolian vowel separator
    {"\xE2\x80\x80", "NQSP", "U+2000"},      // U+2000 : en quad
    {"\xE2\x80\x81", "MQSP", "U+2001"},      // U+2001 : em quad
    {"\xE2\x80\x82", "ENSP", "U+2002"},      // U+2002 : en space
    {"\xE2\x80\x83", "EMSP", "U+2003"},      // U+2003 : em space
    {"\xE2\x80\x84", "3/MSP", "U+2004"},     // U+2004 : three-per-em space
    {"\xE2\x80\x85", "4/MSP", "U+2005"},     // U+2005 : four-per-em space
    {"\xE2\x80\x86", "6/MSP", "U+2006"},     // U+2006 : six-per-em space
    {"\xE2\x80\x87", "FSP", "U+2007"},       // U+2007 : figure space
    {"\xE2\x80\x88", "PSP", "U+2008"},       // U+2008 : punctuation space
    {"\xE2\x80\x89", "THSP", "U+2009"},      // U+2009 : thin space
    {"\xE2\x80\x8A", "HSP", "U+200A"},       // U+200A : hair space
    {"\xE2\x80\x8B", "ZWSP", "U+200B"},      // U+200B : zero-width space
    {"\xE2\x80\x8C", "ZWNJ", "U+200C"},      // U+200C : zero-width non-joiner
    {"\xE2\x80\x8D", "ZWJ", "U+200D"},       // U+200D : zero-width joiner
    {"\xE2\x80\x8E", "LRM", "U+200E"},       // U+200E : left-to-right mark
    {"\xE2\x80\x8F", "RLM", "U+200F"},       // U+200F : right-to-left mark
    {"\xE2\x80\xAA", "LRE", "U+202A"},       // U+202A : left-to-right embedding
    {"\xE2\x80\xAB", "RLE", "U+202B"},       // U+202B : right-to-left embedding
    {"\xE2\x80\xAC", "PDF", "U+202C"},       // U+202C : pop directional formatting
    {"\xE2\x80\xAD", "LRO", "U+202D"},       // U+202D : left-to-right override
    {"\xE2\x80\xAE", "RLO", "U+202E"},       // U+202E : right-to-left override
    {"\xE2\x80\xAF", "NNBSP", "U+202F"},     // U+202F : narrow no-break space
    {"\xE2\x81\x9F", "MMSP", "U+205F"},      // U+205F : medium mathematical space
    {"\xE2\x81\xA0", "WJ", "U+2060"},        // U+2060 : word joiner
    {"\xE2\x81\xA1", "(FA)", "U+2061"},      // U+2061 : function application
    {"\xE2\x81\xA2", "(IT)", "U+2062"},      // U+2062 : invisible times
    {"\xE2\x81\xA3", "(IS)", "U+2063"},      // U+2063 : invisible separator
    {"\xE2\x81\xA4", "(IP)", "U+2064"},      // U+2064 : invisible plus
    {"\xE2\x81\xA6", "LRI", "U+2066"},       // U+2066 : left-to-right isolate
    {"\xE2\x81\xA7", "RLI", "U+2067"},       // U+2067 : right-to-left isolate
    {"\xE2\x81\xA8", "FSI", "U+2068"},       // U+2068 : first strong isolate
    {"\xE2\x81\xA9", "PDI", "U+2069"},       // U+2069 : pop directional isolate
    {"\xE2\x81\xAA", "ISS", "U+206A"},       // U+206A : inhibit symmetric swapping
    {"\xE2\x81\xAB", "ASS", "U+206B"},       // U+206B : activate symmetric swapping
    {"\xE2\x81\xAC", "IAFS", "U+206C"},      // U+206C : inhibit arabic form shaping
    {"\xE2\x81\xAD", "AAFS", "U+206D"},      // U+206D : activate arabic form shaping
    {"\xE2\x81\xAE", "NADS", "U+206E"},      // U+206E : national digit shapes
    {"\xE2\x81\xAF", "NODS", "U+206F"},      // U+206F : nominal digit shapes
    {"\xE3\x80\x80", "IDSP", "U+3000"},      // U+3000 : ideographic space
    {"\xEF\xBB\xBF", "ZWNBSP", "U+FEFF"},    // U+FEFF : zero-width no-break space
    {"\xEF\xBF\xB9", "IAA", "U+FFF9"},       // U+FFF9 : interlinear annotation anchor
    {"\xEF\xBF\xBA", "IAS", "U+FFFA"},       // U+FFFA : interlinear annotation separator
    {"\xEF\xBF\xBB", "IAT", "U+FFFB"}        // U+FFFB : interlinear annotation terminator
} };

constexpr int kMarginLineNumber = 0;
constexpr int kMarginSymbol = 1;
constexpr int kMarginChangeHistory = 2;
constexpr int kMarginFold = 3; // upstream _SC_MARGE_FOLDER

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
    // Upstream "Keep selection when right-click outside of selection":
    // Scintilla moves the caret before its menu opens; put the selection back.
    g_signal_connect(widget_, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* ev, gpointer p) -> gboolean {
                         auto* self = static_cast<EditorView*>(p);
                         if (ev->type != GDK_BUTTON_PRESS || ev->button != 3 || !self->opts_ || !self->opts_->keepSelOnRightClick)
                             return FALSE;
                         const sptr_t textLeft = self->execute(SCI_POINTXFROMPOSITION, 0, 0) + self->execute(SCI_GETXOFFSET);
                         if (ev->x < textLeft || self->execute(SCI_GETSELECTIONEMPTY))
                             return FALSE;
                         self->keptSelection_.clear();
                         const sptr_t n = self->execute(SCI_GETSELECTIONS);
                         for (sptr_t i = 0; i < n; ++i)
                             self->keptSelection_.emplace_back(self->execute(SCI_GETSELECTIONNCARET, static_cast<uptr_t>(i)),
                                                               self->execute(SCI_GETSELECTIONNANCHOR, static_cast<uptr_t>(i)));
                         self->savedSelections_ = static_cast<int>(self->execute(SCI_GETMAINSELECTION));
                         if (self->keepSelIdle_)
                             g_source_remove(self->keepSelIdle_);
                         self->keepSelIdle_ = g_idle_add(+[](gpointer q) -> gboolean {
                             auto* v = static_cast<EditorView*>(q);
                             v->keepSelIdle_ = 0;
                             for (std::size_t i = 0; i < v->keptSelection_.size(); ++i) {
                                 const auto [caret, anchor] = v->keptSelection_[i];
                                 v->execute(i == 0 ? SCI_SETSELECTION : SCI_ADDSELECTION, static_cast<uptr_t>(caret), anchor);
                             }
                             if (!v->keptSelection_.empty())
                                 v->execute(SCI_SETMAINSELECTION, static_cast<uptr_t>(v->savedSelections_));
                             v->keptSelection_.clear();
                             return G_SOURCE_REMOVE;
                         }, self);
                         return FALSE;
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
    if (keepSelIdle_)
        g_source_remove(keepSelIdle_);
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

    execute(SCI_SETMARGINTYPEN, kMarginChangeHistory, SC_MARGIN_SYMBOL);
    execute(SCI_SETMARGINMASKN, kMarginChangeHistory, (1 << SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN) | (1 << SC_MARKNUM_HISTORY_SAVED) |
                                                          (1 << SC_MARKNUM_HISTORY_MODIFIED) | (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED));
    execute(SCI_SETMARGINWIDTHN, kMarginChangeHistory, 0);
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
    for (int ind : {21, 22, 23, 24, 25, 26, 27, 28, 29, 31}) {
        execute(SCI_INDICSETSTYLE, static_cast<uptr_t>(ind), INDIC_ROUNDBOX);
        execute(SCI_INDICSETALPHA, static_cast<uptr_t>(ind), 100);
        execute(SCI_INDICSETOUTLINEALPHA, static_cast<uptr_t>(ind), 150);
        execute(SCI_INDICSETUNDER, static_cast<uptr_t>(ind), true);
        if (const core::Style* s = styles.globalById(ind); s && s->bg)
            execute(SCI_INDICSETFORE, static_cast<uptr_t>(ind), *s->bg);
    }
    // Clickable links: drawn in the text colour, "URL hovered" under the mouse.
    if (auto* s = byName("URL hovered"); s && s->fg)
        execute(SCI_INDICSETHOVERFORE, core::kUrlIndicator, *s->fg);
    execute(SCI_INDICSETFLAGS, core::kUrlIndicator, SC_INDICFLAG_VALUEFORE);
    execute(SCI_INDICSETALPHA, core::kUrlIndicator, 70);
    // Change History markers, as upstream.
    const std::pair<const char*, int> history[] = {{"Change History modified", SC_MARKNUM_HISTORY_MODIFIED},
                                                   {"Change History revert modified", SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED},
                                                   {"Change History revert origin", SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN},
                                                   {"Change History saved", SC_MARKNUM_HISTORY_SAVED}};
    for (const auto& [name, marker] : history)
        if (auto* s = byName(name)) {
            if (s->fg)
                execute(SCI_MARKERSETFORE, static_cast<uptr_t>(marker), *s->fg);
            if (s->bg)
                execute(SCI_MARKERSETBACK, static_cast<uptr_t>(marker), *s->bg);
        }
    if (auto* s = byName("Change History margin"); s && s->bg) {
        execute(SCI_SETMARGINTYPEN, kMarginChangeHistory, SC_MARGIN_COLOUR);
        execute(SCI_SETMARGINBACKN, kMarginChangeHistory, *s->bg);
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
    execute(SCI_SETCHANGEHISTORY, static_cast<uptr_t>(changeHistory_)); // needs the empty undo history
    execute(SCI_GOTOPOS, 0);
    updateLineNumberWidth();
}

void EditorView::applyRepresentations(const Representations& r)
{
    // Upstream showNpc / showCcUniEol / setCRLF / setNpcAndCcUniEOL.
    execute(SCI_CLEARALLREPRESENTATIONS);
    const std::size_t mode = r.npcCodepoint ? 2 : 1;
    const sptr_t npcAppearance = r.npcCustomColor ? SC_REPRESENTATION_BLOB | SC_REPRESENTATION_COLOUR : SC_REPRESENTATION_BLOB;
    auto colour = [&](const char* ch, std::uint32_t bgr) {
        execute(SCI_SETREPRESENTATIONCOLOUR, reinterpret_cast<uptr_t>(ch), static_cast<sptr_t>(bgr | 0xFF000000u));
        execute(SCI_SETREPRESENTATIONAPPEARANCE, reinterpret_cast<uptr_t>(ch), npcAppearance);
    };
    if (r.showCcUniEol) {
        for (const auto& c : g_ccUniEolChars) {
            execute(SCI_SETREPRESENTATION, reinterpret_cast<uptr_t>(c[0]), reinterpret_cast<sptr_t>(c[r.includeCcUniEol ? mode : 1]));
            if (r.includeCcUniEol && r.npcCustomColor)
                colour(c[0], r.npcColor);
        }
    } else {
        for (const auto& c : g_ccUniEolChars) {
            execute(SCI_SETREPRESENTATION, reinterpret_cast<uptr_t>(c[0]), reinterpret_cast<sptr_t>(kZwsp));
            execute(SCI_SETREPRESENTATIONAPPEARANCE, reinterpret_cast<uptr_t>(c[0]), SC_REPRESENTATION_PLAIN);
        }
    }
    if (r.showNpc)
        for (const auto& c : g_nonPrintingChars) {
            execute(SCI_SETREPRESENTATION, reinterpret_cast<uptr_t>(c[0]), reinterpret_cast<sptr_t>(c[mode]));
            if (r.npcCustomColor)
                colour(c[0], r.npcColor);
        }
    const sptr_t eolAppearance = (r.eolPlain ? SC_REPRESENTATION_PLAIN : SC_REPRESENTATION_BLOB) | (r.eolCustomColor ? SC_REPRESENTATION_COLOUR : 0);
    for (const char* ch : {"\r", "\n"}) {
        execute(SCI_SETREPRESENTATIONCOLOUR, reinterpret_cast<uptr_t>(ch), static_cast<sptr_t>(r.eolColor | 0xFF000000u));
        execute(SCI_SETREPRESENTATIONAPPEARANCE, reinterpret_cast<uptr_t>(ch), eolAppearance);
    }
}

void EditorView::refreshLineNumberWidth()
{
    lineNumberDigits_ = -1;
    updateLineNumberWidth();
}

void EditorView::setLinkStyle(bool noUnderline, bool fullBox)
{
    // Upstream addHotSpot: plain underline (or hidden) and EXPLORERLINK / FULLBOX on hover.
    execute(SCI_INDICSETSTYLE, core::kUrlIndicator, noUnderline ? INDIC_HIDDEN : INDIC_PLAIN);
    execute(SCI_INDICSETHOVERSTYLE, core::kUrlIndicator, fullBox ? INDIC_FULLBOX : INDIC_EXPLORERLINK);
}

void EditorView::setChangeHistory(bool margin, bool text)
{
    changeHistory_ = (margin || text ? SC_CHANGE_HISTORY_ENABLED : 0) | (margin ? SC_CHANGE_HISTORY_MARKERS : 0) |
                     (text ? SC_CHANGE_HISTORY_INDICATORS : 0);
    execute(SCI_SETMARGINWIDTHN, kMarginChangeHistory, margin ? 9 : 0);
    // Turning it off (or changing what shows) works at once; turning it on
    // only while there is no undo history.
    if (!changeHistory_ || execute(SCI_GETCHANGEHISTORY) || (!execute(SCI_CANUNDO) && !execute(SCI_CANREDO)))
        execute(SCI_SETCHANGEHISTORY, static_cast<uptr_t>(changeHistory_));
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
    // Upstream: dynamic width follows the last visible line, constant width
    // the line count (at least 4 digits).
    const bool dynamic = !opts_ || opts_->lineNumberDynamic;
    sptr_t lines = lineCount();
    if (dynamic && opts_) {
        const sptr_t lastVisible = execute(SCI_GETFIRSTVISIBLELINE) + execute(SCI_LINESONSCREEN) + 1;
        lines = std::min(lines, execute(SCI_DOCLINEFROMVISIBLE, static_cast<uptr_t>(lastVisible)) + 1);
    }
    int digits = 1;
    for (sptr_t n = lines; n >= 10; n /= 10)
        ++digits;
    digits = std::max(digits, dynamic ? 3 : 4);
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

bool EditorView::tagLanguage() const
{
    if (!lang_)
        return false;
    const std::string& n = lang_->name;
    return n == "html" || n == "xml" || n == "php" || n == "asp" || n == "jsp";
}

int EditorView::markup() const
{
    if (!lang_)
        return 0;
    return lang_->name == "html" ? 1 : lang_->name == "xml" ? 2 : 0;
}

core::IndentStyle EditorView::indentStyle() const
{
    if (!lang_)
        return core::IndentStyle::Plain;
    static const std::set<std::string> cLike = {"c", "cpp", "java", "cs", "objc", "php", "javascript", "javascript.js", "jsp", "css",
                                                "typescript", "go", "swift"};
    static const std::set<std::string> noBraceless = {"perl", "rust", "powershell", "json", "json5"};
    const std::string& n = lang_->name;
    if (n == "python")
        return core::IndentStyle::Python;
    if (noBraceless.count(n))
        return core::IndentStyle::CLikeNoBraceless;
    if (cLike.count(n))
        return core::IndentStyle::CLike;
    return core::IndentStyle::Plain;
}

void EditorView::onCharAdded(int ch)
{
    const core::Sci s = sci();
    // Upstream: control characters (C0) typed by mistake are removed.
    if (opts_->preventC0 && ch > 0 && ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') {
        const sptr_t pos = currentPos();
        if (pos > 0 && execute(SCI_GETCHARAT, static_cast<uptr_t>(pos - 1)) == ch)
            execute(SCI_DELETERANGE, static_cast<uptr_t>(pos - 1), 1);
        return;
    }
    // Several carets: Scintilla types into each; leave the aids out, as upstream.
    if (execute(SCI_GETSELECTIONS) > 1)
        return;
    core::maintainIndentation(s, ch, opts_->autoIndentMode, indentStyle());
    inserter_.onChar(s, ch, opts_->pairs, markup());
    if (!largeFile || opts_->largeAutoComplete)
        completer_.onChar(s, ch, opts_->autoComplete, api_);
}

void EditorView::refreshDecorations()
{
    if (!opts_)
        return;
    const core::Sci s = sci();
    if (opts_->smartHighlight && (!largeFile || opts_->largeSmartHighlight)) {
        EditorView* other = opts_->smartOtherView && otherView ? otherView() : nullptr;
        const core::Sci os = other && other != this ? other->sci() : core::Sci{};
        core::smartHighlight(s, os ? &os : nullptr, opts_->smartWholeWord, opts_->smartMatchCase);
    } else {
        core::clearIndicator(s, core::kSmartIndicator);
    }
    if (opts_->tagMatch && tagLanguage()) {
        core::highlightMatchingTags(s, opts_->tagAttributes, opts_->tagZones, markup() != 2);
    } else {
        core::clearIndicator(s, core::kTagMatchIndicator);
        core::clearIndicator(s, core::kTagAttrIndicator);
    }
    core::markLinks(s, opts_->links && (!largeFile || opts_->largeLinks), opts_->linkSchemes);
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
    case SCN_UPDATEUI: {
        const bool large = largeFile && opts_;
        if (!opts_ || opts_->braceMatch) {
            if (!large || opts_->largeBraceMatch)
                highlightBraces();
            else
                execute(SCI_BRACEHIGHLIGHT, static_cast<uptr_t>(-1), -1);
        }
        // Not SC_UPDATE_CONTENT: marking indicators changes the content flags too.
        if (opts_ && (n.updated & (SC_UPDATE_TEXT | SC_UPDATE_SELECTION | SC_UPDATE_V_SCROLL)))
            refreshDecorations();
        if (opts_ && opts_->lineNumberDynamic && (n.updated & SC_UPDATE_V_SCROLL))
            updateLineNumberWidth();
        if (onUpdateUI)
            onUpdateUI();
        break;
    }
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
        if (opts_)
            onCharAdded(n.ch);
        else if (autoIndent && (n.ch == '\n' || n.ch == '\r'))
            maintainIndentation(n.ch);
        break;
    case SCN_AUTOCSELECTION:
        // Upstream: TAB / ENTER insert the selected item only when allowed.
        if (opts_ && opts_->autoComplete.enable) {
            if (n.listCompletionMethod == SC_AC_NEWLINE && !opts_->acInsertEnter) {
                execute(SCI_AUTOCCANCEL);
                execute(SCI_NEWLINE);
            } else if (n.listCompletionMethod == SC_AC_TAB && !opts_->acInsertTab) {
                execute(SCI_AUTOCCANCEL);
                execute(SCI_TAB);
            }
        }
        break;
    case SCN_CALLTIPCLICK:
        completer_.callTipClick(sci(), static_cast<int>(n.position));
        break;
    case SCN_MARGINCLICK:
        if (n.margin == kMarginSymbol)
            toggleBookmark(lineFromPos(n.position));
        break;
    case SCN_DOUBLECLICK:
        if (opts_ && (n.modifiers & SCMOD_CTRL)) {
            // Upstream: Ctrl + double click selects between the delimiters.
            const sptr_t pos = n.position >= 0 ? n.position : currentPos();
            core::selectBetweenDelimiters(sci(), pos, opts_->delimOpen, opts_->delimClose, opts_->delimMultiLine);
            break;
        }
        if (opts_ && onLinkClicked && n.position >= 0) {
            const std::string url = core::linkAt(sci(), n.position);
            if (!url.empty()) {
                execute(SCI_SETSEL, static_cast<uptr_t>(n.position), n.position);
                onLinkClicked(url);
                break;
            }
        }
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
