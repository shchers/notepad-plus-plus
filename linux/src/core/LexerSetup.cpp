#include "core/LexerSetup.h"

namespace npp::core {

namespace {

enum Kw : int { kInstre1 = 0, kInstre2 = 1, kType1 = 2, kType2 = 3 };

// Upstream concatToBuildKeywordList(): the langs.xml list plus any extra
// keywords the user typed into the Style Configurator for that class.
std::string keywords(const LanguageCatalog& langs, const StyleCatalog& styles, const char* langName, int kwClass)
{
    std::string out;
    if (const Language* l = langs.byName(langName))
        out = l->keywords[kwClass];
    if (const auto* st = styles.forLanguage(langName))
        for (const Style& s : *st)
            if (s.keywordClass == kwClass && !s.userKeywords.empty())
                out += " " + s.userKeywords;
    return out;
}

} // namespace

LexerSetup planLexerSetup(const Language& lang, const LanguageCatalog& langs, const StyleCatalog& styles)
{
    LexerSetup plan;
    const char* self = lang.name.c_str();
    auto kw = [&](const char* name, int cls) { return keywords(langs, styles, name, cls); };

    if (lang.lexer == "cpp") {
        // setCppLexer / setJsLexer / setTypeScriptLexer
        plan.keywordSets[0] = kw(self, kInstre1);
        plan.keywordSets[1] = kw(self, kType1);
        plan.keywordSets[2] = kw("cpp", kType2); // Doxygen keywords always come from C++
        plan.keywordSets[3] = kw(self, kInstre2);
        plan.styleGroups = {lang.name};
    } else if (lang.lexer == "objc") {
        // setObjCLexer
        plan.keywordSets[0] = kw(self, kInstre1);
        plan.keywordSets[1] = kw(self, kType1);
        plan.keywordSets[2] = kw("cpp", kType2);
        plan.keywordSets[3] = kw(self, kInstre2);
        plan.keywordSets[4] = kw(self, kType2);
        plan.styleGroups = {lang.name};
    } else if (lang.lexer == "hypertext") {
        // setXmlLexer(L_HTML/L_PHP/L_ASP/L_JSP) -> setHTMLLexer + embedded JS/PHP/ASP
        plan.keywordSets[0] = kw("html", kInstre1);
        plan.keywordSets[1] = kw("javascript", kInstre1); // L_JS_EMBEDDED
        plan.keywordSets[2] = kw("vb", kInstre1);         // ASP uses VB keywords
        plan.keywordSets[4] = kw("php", kInstre1);
        plan.keywordSets[5] = kw("html", kInstre2);
        plan.styleGroups = {"html", "javascript", "php", "asp"};
    } else if (lang.lexer == "xml") {
        plan.keywordSets[5] = kw(self, kInstre1);
        plan.styleGroups = {lang.name};
    } else {
        for (int i = 0; i < kKeywordSets; ++i)
            plan.keywordSets[i] = kw(self, i);
        plan.styleGroups = {lang.name};
    }
    return plan;
}

} // namespace npp::core
