// Language definitions — a UI-free subset of upstream NppParameters' handling
// of langs.model.xml (Lang / feedKeyWordsParameters) merged with the
// generated copy of ScintillaEditView::_langNameInfoArray.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace npp::core {

// Matches upstream LANG_INDEX_INSTR .. LANG_INDEX_TYPE7: keyword class index
// N is passed to SCI_SETKEYWORDS(N, ...).
inline constexpr int kKeywordSets = 9;

struct Language {
    std::string name;      // "cpp", "python", ... (langs.model.xml / stylers.model.xml key)
    std::string shortName; // "C++"
    std::string longName;  // "C++ source file" (status bar)
    std::string lexer;     // Lexilla lexer name, "null" for plain text
    std::vector<std::string> extensions; // lower case, without dot
    std::array<std::string, kKeywordSets> keywords;
    std::string commentLine, commentStart, commentEnd;

    bool isPlainText() const { return lexer == "null"; }
};

class LanguageCatalog {
public:
    // Loads langs.model.xml (or a user langs.xml with the same schema).
    bool load(const std::filesystem::path& langsXml, std::string* error = nullptr);

    const std::vector<Language>& all() const { return languages_; }
    const Language* byName(std::string_view name) const;
    // Lookup by Notepad++ UI short name ("C++") or name ("cpp"), case-insensitive.
    // Used by the -l<lang> command line switch.
    const Language* byAnyName(std::string_view name) const;
    // Picks a language from the file name, as Notepad++ does on open.
    const Language& forFile(const std::filesystem::path& file) const;
    const Language& plainText() const;

    // Maps "instre1".."type7" (or "0".."8") to a keyword set index, -1 if unknown.
    static int keywordClassIndex(std::string_view kwClass);

private:
    std::vector<Language> languages_;
};

} // namespace npp::core
