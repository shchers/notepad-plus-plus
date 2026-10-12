#include "ui/CharPanel.h"

#include "platform/TextCodec.h"

#include <glib.h>

namespace npp::ui {

namespace {

enum Col { kColValue, kColHex, kColChar, kColHtmlName, kColHtmlDec, kColHtmlHex, kColText, kColCount };

// Upstream asciiListView names for the control codes.
const char* const kControl[] = {"NULL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS",  "TAB", "LF",  "VT",  "FF",  "CR",  "SO",  "SI",
                                "DLE",  "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US"};

// HTML entity names by Unicode code point (Latin-1 and the Windows-1252 extras).
const char* htmlName(gunichar c)
{
    static const struct { gunichar c; const char* n; } kNames[] = {
        {'"', "&quot;"}, {'&', "&amp;"}, {'<', "&lt;"}, {'>', "&gt;"}, {0xA0, "&nbsp;"}, {0xA1, "&iexcl;"}, {0xA2, "&cent;"},
        {0xA3, "&pound;"}, {0xA4, "&curren;"}, {0xA5, "&yen;"}, {0xA6, "&brvbar;"}, {0xA7, "&sect;"}, {0xA8, "&uml;"}, {0xA9, "&copy;"},
        {0xAA, "&ordf;"}, {0xAB, "&laquo;"}, {0xAC, "&not;"}, {0xAD, "&shy;"}, {0xAE, "&reg;"}, {0xAF, "&macr;"}, {0xB0, "&deg;"},
        {0xB1, "&plusmn;"}, {0xB2, "&sup2;"}, {0xB3, "&sup3;"}, {0xB4, "&acute;"}, {0xB5, "&micro;"}, {0xB6, "&para;"}, {0xB7, "&middot;"},
        {0xB8, "&cedil;"}, {0xB9, "&sup1;"}, {0xBA, "&ordm;"}, {0xBB, "&raquo;"}, {0xBC, "&frac14;"}, {0xBD, "&frac12;"}, {0xBE, "&frac34;"},
        {0xBF, "&iquest;"}, {0xC0, "&Agrave;"}, {0xC1, "&Aacute;"}, {0xC2, "&Acirc;"}, {0xC3, "&Atilde;"}, {0xC4, "&Auml;"}, {0xC5, "&Aring;"},
        {0xC6, "&AElig;"}, {0xC7, "&Ccedil;"}, {0xC8, "&Egrave;"}, {0xC9, "&Eacute;"}, {0xCA, "&Ecirc;"}, {0xCB, "&Euml;"}, {0xCC, "&Igrave;"},
        {0xCD, "&Iacute;"}, {0xCE, "&Icirc;"}, {0xCF, "&Iuml;"}, {0xD0, "&ETH;"}, {0xD1, "&Ntilde;"}, {0xD2, "&Ograve;"}, {0xD3, "&Oacute;"},
        {0xD4, "&Ocirc;"}, {0xD5, "&Otilde;"}, {0xD6, "&Ouml;"}, {0xD7, "&times;"}, {0xD8, "&Oslash;"}, {0xD9, "&Ugrave;"}, {0xDA, "&Uacute;"},
        {0xDB, "&Ucirc;"}, {0xDC, "&Uuml;"}, {0xDD, "&Yacute;"}, {0xDE, "&THORN;"}, {0xDF, "&szlig;"}, {0xE0, "&agrave;"}, {0xE1, "&aacute;"},
        {0xE2, "&acirc;"}, {0xE3, "&atilde;"}, {0xE4, "&auml;"}, {0xE5, "&aring;"}, {0xE6, "&aelig;"}, {0xE7, "&ccedil;"}, {0xE8, "&egrave;"},
        {0xE9, "&eacute;"}, {0xEA, "&ecirc;"}, {0xEB, "&euml;"}, {0xEC, "&igrave;"}, {0xED, "&iacute;"}, {0xEE, "&icirc;"}, {0xEF, "&iuml;"},
        {0xF0, "&eth;"}, {0xF1, "&ntilde;"}, {0xF2, "&ograve;"}, {0xF3, "&oacute;"}, {0xF4, "&ocirc;"}, {0xF5, "&otilde;"}, {0xF6, "&ouml;"},
        {0xF7, "&divide;"}, {0xF8, "&oslash;"}, {0xF9, "&ugrave;"}, {0xFA, "&uacute;"}, {0xFB, "&ucirc;"}, {0xFC, "&uuml;"}, {0xFD, "&yacute;"},
        {0xFE, "&thorn;"}, {0xFF, "&yuml;"}, {0x20AC, "&euro;"}, {0x201A, "&sbquo;"}, {0x192, "&fnof;"}, {0x201E, "&bdquo;"},
        {0x2026, "&hellip;"}, {0x2020, "&dagger;"}, {0x2021, "&Dagger;"}, {0x2C6, "&circ;"}, {0x2030, "&permil;"}, {0x160, "&Scaron;"},
        {0x2039, "&lsaquo;"}, {0x152, "&OElig;"}, {0x17D, "&Zcaron;"}, {0x2018, "&lsquo;"}, {0x2019, "&rsquo;"}, {0x201C, "&ldquo;"},
        {0x201D, "&rdquo;"}, {0x2022, "&bull;"}, {0x2013, "&ndash;"}, {0x2014, "&mdash;"}, {0x2DC, "&tilde;"}, {0x2122, "&trade;"},
        {0x161, "&scaron;"}, {0x203A, "&rsaquo;"}, {0x153, "&oelig;"}, {0x17E, "&zcaron;"}, {0x178, "&Yuml;"},
    };
    for (const auto& e : kNames)
        if (e.c == c)
            return e.n;
    return "";
}

} // namespace

CharPanel::CharPanel(const core::Translator& tr, std::function<void(const std::string&)> insert) : insert_(std::move(insert))
{
    box_ = gtk_scrolled_window_new(nullptr, nullptr);
    g_object_ref_sink(box_);
    store_ = gtk_list_store_new(kColCount, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    const char* keys[][2] = {{"ColumnVal", "Value"},        {"ColumnHex", "Hex"},           {"ColumnChar", "Character"},
                             {"ColumnHtmlName", "HTML Name"}, {"ColumnHtmlNumber", "HTML Decimal"}, {"ColumnHtmlHexNb", "HTML Hexadecimal"}};
    for (int c = 0; c < 6; ++c) {
        GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
            tr.dialogAttr(std::string("AsciiInsertion/") + keys[c][0], "name", keys[c][1]).c_str(), gtk_cell_renderer_text_new(), "text", c, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        g_object_set_data(G_OBJECT(col), "npp-col", GINT_TO_POINTER(c));
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), col);
    }
    gtk_container_add(GTK_CONTAINER(box_), tree_);
    g_signal_connect(tree_, "row-activated", G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn* col, gpointer p) {
                         auto* self = static_cast<CharPanel*>(p);
                         const int code = gtk_tree_path_get_indices(path)[0];
                         const int c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "npp-col"));
                         if (self->insert_)
                             self->insert_(self->textFor(code, c));
                     }), this);
    setCharset("");
}

CharPanel::~CharPanel()
{
    g_object_unref(store_);
    g_object_unref(box_);
}

void CharPanel::setCharset(const std::string& cs)
{
    const bool unicode = cs.empty() || g_ascii_strncasecmp(cs.c_str(), "UTF", 3) == 0;
    const std::string charset = unicode ? "WINDOWS-1252" : cs;
    if (charset == charset_ && gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store_), nullptr) == 256)
        return;
    charset_ = charset;
    gtk_list_store_clear(store_);
    for (int i = 0; i < 256; ++i) {
        std::string ch;
        if (i < 32)
            ch = kControl[i];
        else if (i == 127)
            ch = "DEL";
        else if (i < 128)
            ch = std::string(1, static_cast<char>(i));
        else if (auto d = platform::decodeAs(std::string(1, static_cast<char>(i)), charset_); d && !d->utf8.empty())
            ch = d->utf8;
        const gunichar u = i < 128 ? static_cast<gunichar>(i) : (ch.empty() ? 0 : g_utf8_get_char_validated(ch.c_str(), -1));
        char hex[8], dec[16], hhex[16];
        g_snprintf(hex, sizeof hex, "%02X", i);
        g_snprintf(dec, sizeof dec, "&#%u;", u);
        g_snprintf(hhex, sizeof hhex, "&#x%X;", u);
        // Character to insert: the code point (control codes as themselves).
        char buf[8] = {};
        if (u)
            buf[g_unichar_to_utf8(u, buf)] = '\0';
        const bool noChar = (i >= 128 && ch.empty()) || i == 0;
        gtk_list_store_insert_with_values(store_, nullptr, -1, kColValue, i, kColHex, hex, kColChar, ch.c_str(), kColHtmlName,
                                          (i >= 32 && u) ? htmlName(u) : "", kColHtmlDec, noChar || i < 32 ? "" : dec, kColHtmlHex,
                                          noChar || i < 32 ? "" : hhex, kColText, noChar ? "" : buf, -1);
    }
}

std::string CharPanel::textFor(int code, int col) const
{
    GtkTreeIter it;
    if (!gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store_), &it, nullptr, code))
        return {};
    const int column = col >= kColHtmlName && col <= kColHtmlHex ? col : kColText;
    gchar* s = nullptr;
    gtk_tree_model_get(GTK_TREE_MODEL(store_), &it, column, &s, -1);
    std::string out = s ? s : "";
    g_free(s);
    // An HTML column without a value inserts the character, as upstream.
    if (out.empty() && column != kColText)
        return textFor(code, kColValue);
    return out;
}

} // namespace npp::ui
