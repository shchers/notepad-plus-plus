// File encoding detection and conversion — replaces upstream Utf8_16 +
// FileManager::detectCodepage (uchardet) + Win32 MultiByteToWideChar with
// uchardet + GLib g_convert (iconv). Scintilla always holds UTF-8.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace npp::platform {

enum class Bom { None, Utf8, Utf16LE, Utf16BE };

struct TextEncoding {
    std::string charset = "UTF-8"; // iconv name of the on-disk bytes (sans BOM)
    Bom bom = Bom::None;

    bool operator==(const TextEncoding&) const = default;
    // Notepad++ style label: "UTF-8", "UTF-8-BOM", "UTF-16 LE BOM", "ISO-8859-1"...
    std::string displayName() const;
};

struct DecodedText {
    std::string utf8;
    TextEncoding encoding;
    bool guessed = false; // detection failed; opened as ISO-8859-1 (byte-exact round trip)
};

// Detect encoding of raw file bytes and convert to UTF-8.
DecodedText decode(std::string_view raw);

// Convert UTF-8 editor text back to the given encoding, prepending its BOM.
// Returns std::nullopt (and fills *error) if the text cannot be represented.
std::optional<std::string> encode(std::string_view utf8, const TextEncoding& enc,
                                  std::string* error = nullptr);

} // namespace npp::platform
