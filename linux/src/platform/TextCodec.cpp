#include "platform/TextCodec.h"

#include <glib.h>
#include <uchardet.h>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <memory>

namespace npp::platform {

namespace {

constexpr std::string_view kBomUtf8{"\xEF\xBB\xBF", 3};
constexpr std::string_view kBomUtf16LE{"\xFF\xFE", 2};
constexpr std::string_view kBomUtf16BE{"\xFE\xFF", 2};

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::optional<std::string> convert(std::string_view in, const char* to, const char* from, std::string* error)
{
    GError* err = nullptr;
    gsize written = 0;
    std::unique_ptr<gchar, decltype(&g_free)> out(
        g_convert(in.data(), static_cast<gssize>(in.size()), to, from, nullptr, &written, &err), &g_free);
    if (!out) {
        if (error)
            *error = err ? err->message : "conversion failed";
        if (err)
            g_error_free(err);
        return std::nullopt;
    }
    return std::string(out.get(), written);
}

// Same policy as upstream FileManager::detectCodepage: trust uchardet except
// for TIS-620, which it reports wrongly for UTF-8 too often.
std::string detectCharset(std::string_view raw)
{
    uchardet_t ud = uchardet_new();
    uchardet_handle_data(ud, raw.data(), raw.size());
    uchardet_data_end(ud);
    std::string cs = uchardet_get_charset(ud);
    uchardet_delete(ud);
    if (upper(cs) == "TIS-620")
        cs.clear();
    return cs;
}

// Upstream EncodingMapper: code page, then the iconv names (first one is used
// for conversion; the others are aliases uchardet or users may report).
struct CodepageNames {
    int codepage;
    std::initializer_list<std::string_view> names;
};
const CodepageNames kCodepages[] = {
    {1250, {"WINDOWS-1250", "CP1250"}},
    {1251, {"WINDOWS-1251", "CP1251"}},
    {1252, {"WINDOWS-1252", "CP1252"}},
    {1253, {"WINDOWS-1253", "CP1253"}},
    {1254, {"WINDOWS-1254", "CP1254"}},
    {1255, {"WINDOWS-1255", "CP1255"}},
    {1256, {"WINDOWS-1256", "CP1256"}},
    {1257, {"WINDOWS-1257", "CP1257"}},
    {1258, {"WINDOWS-1258", "CP1258"}},
    {28591, {"ISO-8859-1", "ISO_8859-1", "LATIN1", "L1"}},
    {28592, {"ISO-8859-2", "ISO_8859-2", "LATIN2", "L2"}},
    {28593, {"ISO-8859-3", "ISO_8859-3", "LATIN3", "L3"}},
    {28594, {"ISO-8859-4", "ISO_8859-4", "LATIN4", "L4"}},
    {28595, {"ISO-8859-5", "ISO_8859-5", "CYRILLIC"}},
    {28596, {"ISO-8859-6", "ISO_8859-6", "ARABIC"}},
    {28597, {"ISO-8859-7", "ISO_8859-7", "GREEK"}},
    {28598, {"ISO-8859-8", "ISO_8859-8", "HEBREW"}},
    {28599, {"ISO-8859-9", "ISO_8859-9", "LATIN5", "L5"}},
    {28603, {"ISO-8859-13", "ISO_8859-13"}},
    {28604, {"ISO-8859-14", "ISO_8859-14"}},
    {28605, {"ISO-8859-15", "ISO_8859-15", "LATIN-9"}},
    {437, {"IBM437", "CP437"}},
    {737, {"CP737", "IBM737"}},
    {775, {"CP775", "IBM775"}},
    {850, {"IBM850", "CP850"}},
    {852, {"IBM852", "CP852"}},
    {855, {"IBM855", "CP855"}},
    {857, {"IBM857", "CP857"}},
    {860, {"IBM860", "CP860"}},
    {861, {"IBM861", "CP861"}},
    {862, {"IBM862", "CP862"}},
    {863, {"IBM863", "CP863"}},
    {865, {"IBM865", "CP865"}},
    {866, {"IBM866", "CP866"}},
    {869, {"IBM869", "CP869"}},
    {950, {"BIG5", "CP950"}},
    {936, {"GB18030", "GBK", "GB2312", "CP936"}},
    {932, {"SHIFT_JIS", "CP932", "MS_KANJI"}},
    {949, {"CP949", "UHC"}},
    {51949, {"EUC-KR"}},
    {874, {"TIS-620", "CP874"}},
    {10007, {"MAC-CYRILLIC", "MACCYRILLIC", "X-MAC-CYRILLIC"}},
    {21866, {"KOI8-U"}},
    {20866, {"KOI8-R"}},
};

} // namespace

int codepageForCharset(std::string_view charset)
{
    const std::string cs = upper(std::string(charset));
    for (const auto& e : kCodepages)
        for (std::string_view n : e.names)
            if (cs == n)
                return e.codepage;
    return -1;
}

std::string charsetForCodepage(int codepage)
{
    for (const auto& e : kCodepages)
        if (e.codepage == codepage)
            return std::string(*e.names.begin());
    return {};
}

std::string TextEncoding::displayName() const
{
    switch (bom) {
    case Bom::Utf8: return "UTF-8-BOM";
    case Bom::Utf16LE: return "UTF-16 LE BOM";
    case Bom::Utf16BE: return "UTF-16 BE BOM";
    case Bom::None: break;
    }
    return upper(charset);
}

DecodedText decode(std::string_view raw)
{
    DecodedText r;
    auto withBom = [&](std::string_view bom, Bom kind, const char* charset) -> bool {
        if (!raw.starts_with(bom))
            return false;
        std::string_view body = raw.substr(bom.size());
        if (kind == Bom::Utf8) {
            r.utf8.assign(body);
        } else if (auto u = convert(body, "UTF-8", charset, nullptr)) {
            r.utf8 = std::move(*u);
        } else {
            return false; // malformed UTF-16 (e.g. odd length): use generic detection
        }
        r.encoding = {charset, kind};
        return true;
    };
    if (withBom(kBomUtf8, Bom::Utf8, "UTF-8") || withBom(kBomUtf16LE, Bom::Utf16LE, "UTF-16LE") ||
        withBom(kBomUtf16BE, Bom::Utf16BE, "UTF-16BE"))
        return r;

    if (raw.empty() || g_utf8_validate(raw.data(), static_cast<gssize>(raw.size()), nullptr)) {
        r.utf8.assign(raw);
        r.encoding = {"UTF-8", Bom::None};
        return r;
    }

    std::string cs = detectCharset(raw);
    if (!cs.empty() && upper(cs) != "ASCII" && upper(cs) != "UTF-8") {
        if (auto u = convert(raw, "UTF-8", cs.c_str(), nullptr)) {
            r.utf8 = std::move(*u);
            r.encoding = {cs, Bom::None};
            return r;
        }
    }
    // Last resort: ISO-8859-1 maps every byte, so the file always opens and
    // round-trips byte-exact on save.
    r.utf8 = convert(raw, "UTF-8", "ISO-8859-1", nullptr).value_or(std::string(raw));
    r.encoding = {"ISO-8859-1", Bom::None};
    r.guessed = true;
    return r;
}

std::optional<DecodedText> decodeAs(std::string_view raw, const std::string& charset, std::string* error)
{
    const std::string cs = upper(charset);
    if (raw.starts_with(kBomUtf8) || raw.starts_with(kBomUtf16LE) || raw.starts_with(kBomUtf16BE) || cs == "UTF-8")
        return decode(raw);
    auto u = convert(raw, "UTF-8", charset.c_str(), error);
    if (!u)
        return std::nullopt;
    DecodedText r;
    r.utf8 = std::move(*u);
    r.encoding = {charset, Bom::None};
    return r;
}

std::optional<std::string> encode(std::string_view utf8, const TextEncoding& enc, std::string* error)
{
    std::string out;
    switch (enc.bom) {
    case Bom::Utf8: out.assign(kBomUtf8); break;
    case Bom::Utf16LE: out.assign(kBomUtf16LE); break;
    case Bom::Utf16BE: out.assign(kBomUtf16BE); break;
    case Bom::None: break;
    }
    if (upper(enc.charset) == "UTF-8") {
        out.append(utf8);
        return out;
    }
    auto body = convert(utf8, enc.charset.c_str(), "UTF-8", error);
    if (!body)
        return std::nullopt;
    out += *body;
    return out;
}

} // namespace npp::platform
