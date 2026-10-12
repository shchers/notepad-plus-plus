// User Defined Languages — upstream's userDefineLang.xml / userDefineLangs/*.xml
// (NppParameters::feedUserLang, feedUserSettings, feedUserKeywordList,
// feedUserStyles) and the LexUser setup of ScintillaEditView::setUserLexer,
// without Win32. The files are read unmodified, older UDL versions included.
#pragma once

#include "core/StyleCatalog.h"

#include <SciLexer.h>

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace npp::core {

struct UserLanguage {
    std::string name;
    std::string ext;         // space separated extensions, no dots
    std::string udlVersion;
    bool darkModeTheme = false;
    std::filesystem::path file;

    bool caseIgnored = false;
    bool allowFoldOfComments = false;
    bool foldCompact = false;
    int forcePureLC = 0;
    int decimalSeparator = 0;
    std::array<bool, SCE_USER_TOTAL_KEYWORD_GROUPS> isPrefix{};
    std::array<std::string, SCE_USER_KWLIST_TOTAL> keywordLists{};
    // One per style ID 0 .. SCE_USER_STYLE_TOTAL_STYLES - 1 (DEFAULT, COMMENTS, ...).
    std::vector<Style> styles;
    std::vector<int> nesting; // per style ID

    bool matchesExtension(std::string_view fileExt) const; // "md" or ".md"
};

// All <UserLang> elements of a file (a file may hold several). Bad entries
// are skipped, as upstream does.
std::vector<UserLanguage> loadUserLanguages(const std::filesystem::path& file, std::string* error = nullptr);
// userDefineLang.xml in `configDir`, then every *.xml in each of `dirs`;
// later definitions of a name are ignored.
std::vector<UserLanguage> loadAllUserLanguages(const std::filesystem::path& configDir, const std::vector<std::filesystem::path>& dirs);

// What setUserLexer sends to the "user" lexer.
struct UserLexerSetup {
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<std::string> keywordSets; // SCI_SETKEYWORDS 0..n-1
    std::vector<Style> styles;
};
// `udlId` / `bufferId` identify the language and the document for LexUser's
// nesting cache (upstream passes pointers).
UserLexerSetup planUserLexer(const UserLanguage& lang, long udlId, long bufferId);

} // namespace npp::core
