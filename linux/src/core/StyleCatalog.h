// Style definitions from stylers.model.xml (or a theme XML with the same
// schema) — UI-free subset of upstream NppParameters::feedStylerArray.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace npp::core {

// Upstream fontStyle bits (FONTSTYLE_BOLD/ITALIC/UNDERLINE).
enum FontStyleBits : int { kBold = 1, kItalic = 2, kUnderline = 4 };

struct Style {
    std::string name;
    int id = 0;
    std::optional<std::uint32_t> fg; // Scintilla BGR colour
    std::optional<std::uint32_t> bg;
    std::string fontName;            // empty = inherit
    std::optional<int> fontStyle;    // FontStyleBits
    std::optional<int> fontSize;     // points
    int keywordClass = -1;           // keyword set this style colours, -1 if none
    std::string userKeywords;        // extra keywords stored in the element text
};

class StyleCatalog {
public:
    struct LexerInfo {
        std::string name; // "cpp"
        std::string desc; // "C++"
        std::string ext;  // user extensions (Style Configurator "User ext.")
    };

    bool load(const std::filesystem::path& stylersXml, std::string* error = nullptr);
    // Writes the styles into a copy of the file they were loaded from, so
    // everything the port does not model is kept (upstream WordStyleDlg Save).
    bool save(const std::filesystem::path& target, std::string* error = nullptr) const;
    const std::filesystem::path& source() const { return source_; }

    // Lexers in file order, and editable style lists (Style Configurator).
    const std::vector<LexerInfo>& lexers() const { return lexerInfo_; }
    std::vector<Style>* mutableLexer(std::string_view name);
    std::vector<Style>& mutableGlobals() { return globals_; }
    void setLexerExt(std::string_view name, std::string ext);

    // Scintilla 0xBBGGRR -> "RRGGBB".
    static std::string toHex(std::uint32_t bgr);

    // Styles for a lexer keyed by Notepad++ language name ("cpp"); nullptr if none.
    const std::vector<Style>* forLanguage(std::string_view langName) const;
    const std::vector<Style>& globals() const { return globals_; }
    const Style* global(std::string_view name) const;
    const Style* globalById(int id) const;

    // "RRGGBB" -> Scintilla 0xBBGGRR.
    static std::optional<std::uint32_t> parseColour(std::string_view hex);

private:
    std::unordered_map<std::string, std::vector<Style>> lexers_;
    std::vector<Style> globals_;
    std::vector<LexerInfo> lexerInfo_;
    std::filesystem::path source_;
};

} // namespace npp::core
