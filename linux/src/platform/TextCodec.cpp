#include "platform/TextCodec.h"

#include <glib.h>
#include <uchardet.h>

#include <algorithm>
#include <cctype>
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

} // namespace

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
