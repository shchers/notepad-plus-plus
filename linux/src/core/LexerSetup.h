// Which keyword list goes into which Scintilla keyword set, and which style
// groups to apply — the data-level part of upstream
// ScintillaEditView::defineDocType() / set*Lexer().
//
// Most lexers use upstream's generic setLexer(): keyword class N ("instre1"=0
// ... "type7"=8) goes to SCI_SETKEYWORDS(N). The exceptions reproduced here:
//   cpp family  (C, C++, C#, Java, RC, ActionScript, Swift, Go, JS, TypeScript)
//   Objective-C, HTML/PHP/ASP/JSP (hypertext + embedded JS/PHP/VBScript), XML.
#pragma once

#include "core/LanguageCatalog.h"
#include "core/StyleCatalog.h"

#include <array>
#include <string>
#include <vector>

namespace npp::core {

struct LexerSetup {
    std::array<std::string, kKeywordSets> keywordSets; // SCI_SETKEYWORDS(i, keywordSets[i])
    std::vector<std::string> styleGroups;              // stylers.xml LexerType names, applied in order
};

LexerSetup planLexerSetup(const Language& lang, const LanguageCatalog& langs, const StyleCatalog& styles);

} // namespace npp::core
