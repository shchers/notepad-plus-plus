#include "core/UserLanguage.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

// Upstream GlobalMappers (UserDefineDialog.h): keyword list and style names
// of every UDL version, newest last so they win.
const std::map<std::string, int>& keywordIds()
{
    static const std::map<std::string, int> m = {
        // pre 2.0
        {"Operators", SCE_USER_KWLIST_OPERATORS1}, {"Folder+", SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN},
        {"Folder-", SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE}, {"Words1", SCE_USER_KWLIST_KEYWORDS1},
        {"Words2", SCE_USER_KWLIST_KEYWORDS2}, {"Words3", SCE_USER_KWLIST_KEYWORDS3}, {"Words4", SCE_USER_KWLIST_KEYWORDS4},
        // 2.0
        {"Comments", SCE_USER_KWLIST_COMMENTS}, {"Numbers, additional", SCE_USER_KWLIST_NUMBER_RANGE},
        {"Numbers, prefixes", SCE_USER_KWLIST_NUMBER_PREFIX2}, {"Numbers, extras with prefixes", SCE_USER_KWLIST_NUMBER_EXTRAS2},
        {"Numbers, suffixes", SCE_USER_KWLIST_NUMBER_SUFFIX2}, {"Operators1", SCE_USER_KWLIST_OPERATORS1},
        {"Operators2", SCE_USER_KWLIST_OPERATORS2}, {"Folders in code1, open", SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN},
        {"Folders in code1, middle", SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE}, {"Folders in code1, close", SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE},
        {"Folders in code2, open", SCE_USER_KWLIST_FOLDERS_IN_CODE2_OPEN}, {"Folders in code2, middle", SCE_USER_KWLIST_FOLDERS_IN_CODE2_MIDDLE},
        {"Folders in code2, close", SCE_USER_KWLIST_FOLDERS_IN_CODE2_CLOSE},
        {"Folders in comment, open", SCE_USER_KWLIST_FOLDERS_IN_COMMENT_OPEN},
        {"Folders in comment, middle", SCE_USER_KWLIST_FOLDERS_IN_COMMENT_MIDDLE},
        {"Folders in comment, close", SCE_USER_KWLIST_FOLDERS_IN_COMMENT_CLOSE},
        {"Keywords1", SCE_USER_KWLIST_KEYWORDS1}, {"Keywords2", SCE_USER_KWLIST_KEYWORDS2}, {"Keywords3", SCE_USER_KWLIST_KEYWORDS3},
        {"Keywords4", SCE_USER_KWLIST_KEYWORDS4}, {"Keywords5", SCE_USER_KWLIST_KEYWORDS5}, {"Keywords6", SCE_USER_KWLIST_KEYWORDS6},
        {"Keywords7", SCE_USER_KWLIST_KEYWORDS7}, {"Keywords8", SCE_USER_KWLIST_KEYWORDS8}, {"Delimiters", SCE_USER_KWLIST_DELIMITERS},
        // 2.1
        {"Numbers, prefix1", SCE_USER_KWLIST_NUMBER_PREFIX1}, {"Numbers, prefix2", SCE_USER_KWLIST_NUMBER_PREFIX2},
        {"Numbers, extras1", SCE_USER_KWLIST_NUMBER_EXTRAS1}, {"Numbers, extras2", SCE_USER_KWLIST_NUMBER_EXTRAS2},
        {"Numbers, suffix1", SCE_USER_KWLIST_NUMBER_SUFFIX1}, {"Numbers, suffix2", SCE_USER_KWLIST_NUMBER_SUFFIX2},
        {"Numbers, range", SCE_USER_KWLIST_NUMBER_RANGE},
    };
    return m;
}

const std::map<std::string, int>& styleIds()
{
    static const std::map<std::string, int> m = {
        // pre 2.0
        {"FOLDEROPEN", SCE_USER_STYLE_FOLDER_IN_CODE1}, {"FOLDERCLOSE", SCE_USER_STYLE_FOLDER_IN_CODE1},
        {"KEYWORD1", SCE_USER_STYLE_KEYWORD1}, {"KEYWORD2", SCE_USER_STYLE_KEYWORD2}, {"KEYWORD3", SCE_USER_STYLE_KEYWORD3},
        {"KEYWORD4", SCE_USER_STYLE_KEYWORD4}, {"COMMENT", SCE_USER_STYLE_COMMENT}, {"COMMENT LINE", SCE_USER_STYLE_COMMENTLINE},
        {"NUMBER", SCE_USER_STYLE_NUMBER}, {"OPERATOR", SCE_USER_STYLE_OPERATOR}, {"DELIMINER1", SCE_USER_STYLE_DELIMITER1},
        {"DELIMINER2", SCE_USER_STYLE_DELIMITER2}, {"DELIMINER3", SCE_USER_STYLE_DELIMITER3},
        // post 2.0
        {"DEFAULT", SCE_USER_STYLE_DEFAULT}, {"COMMENTS", SCE_USER_STYLE_COMMENT}, {"LINE COMMENTS", SCE_USER_STYLE_COMMENTLINE},
        {"NUMBERS", SCE_USER_STYLE_NUMBER}, {"KEYWORDS1", SCE_USER_STYLE_KEYWORD1}, {"KEYWORDS2", SCE_USER_STYLE_KEYWORD2},
        {"KEYWORDS3", SCE_USER_STYLE_KEYWORD3}, {"KEYWORDS4", SCE_USER_STYLE_KEYWORD4}, {"KEYWORDS5", SCE_USER_STYLE_KEYWORD5},
        {"KEYWORDS6", SCE_USER_STYLE_KEYWORD6}, {"KEYWORDS7", SCE_USER_STYLE_KEYWORD7}, {"KEYWORDS8", SCE_USER_STYLE_KEYWORD8},
        {"OPERATORS", SCE_USER_STYLE_OPERATOR}, {"FOLDER IN CODE1", SCE_USER_STYLE_FOLDER_IN_CODE1},
        {"FOLDER IN CODE2", SCE_USER_STYLE_FOLDER_IN_CODE2}, {"FOLDER IN COMMENT", SCE_USER_STYLE_FOLDER_IN_COMMENT},
        {"DELIMITERS1", SCE_USER_STYLE_DELIMITER1}, {"DELIMITERS2", SCE_USER_STYLE_DELIMITER2}, {"DELIMITERS3", SCE_USER_STYLE_DELIMITER3},
        {"DELIMITERS4", SCE_USER_STYLE_DELIMITER4}, {"DELIMITERS5", SCE_USER_STYLE_DELIMITER5}, {"DELIMITERS6", SCE_USER_STYLE_DELIMITER6},
        {"DELIMITERS7", SCE_USER_STYLE_DELIMITER7}, {"DELIMITERS8", SCE_USER_STYLE_DELIMITER8},
    };
    return m;
}

// Lists LexUser takes as properties; the others are SCI_SETKEYWORDS sets.
const std::map<int, const char*>& propertyLists()
{
    static const std::map<int, const char*> m = {
        {SCE_USER_KWLIST_COMMENTS, "userDefine.comments"},
        {SCE_USER_KWLIST_DELIMITERS, "userDefine.delimiters"},
        {SCE_USER_KWLIST_OPERATORS1, "userDefine.operators1"},
        {SCE_USER_KWLIST_NUMBER_PREFIX1, "userDefine.numberPrefix1"},
        {SCE_USER_KWLIST_NUMBER_PREFIX2, "userDefine.numberPrefix2"},
        {SCE_USER_KWLIST_NUMBER_EXTRAS1, "userDefine.numberExtras1"},
        {SCE_USER_KWLIST_NUMBER_EXTRAS2, "userDefine.numberExtras2"},
        {SCE_USER_KWLIST_NUMBER_SUFFIX1, "userDefine.numberSuffix1"},
        {SCE_USER_KWLIST_NUMBER_SUFFIX2, "userDefine.numberSuffix2"},
        {SCE_USER_KWLIST_NUMBER_RANGE, "userDefine.numberRange"},
        {SCE_USER_KWLIST_FOLDERS_IN_CODE1_OPEN, "userDefine.foldersInCode1Open"},
        {SCE_USER_KWLIST_FOLDERS_IN_CODE1_MIDDLE, "userDefine.foldersInCode1Middle"},
        {SCE_USER_KWLIST_FOLDERS_IN_CODE1_CLOSE, "userDefine.foldersInCode1Close"},
    };
    return m;
}

const char* styleName(int id)
{
    static const char* const names[] = {"DEFAULT", "COMMENTS", "LINE COMMENTS", "NUMBERS", "KEYWORDS1", "KEYWORDS2", "KEYWORDS3",
                                        "KEYWORDS4", "KEYWORDS5", "KEYWORDS6", "KEYWORDS7", "KEYWORDS8", "OPERATORS",
                                        "FOLDER IN CODE1", "FOLDER IN CODE2", "FOLDER IN COMMENT", "DELIMITERS1", "DELIMITERS2",
                                        "DELIMITERS3", "DELIMITERS4", "DELIMITERS5", "DELIMITERS6", "DELIMITERS7", "DELIMITERS8"};
    return id >= 0 && id < static_cast<int>(std::size(names)) ? names[id] : "";
}

bool yes(const pugi::xml_node& n, const char* attr)
{
    const std::string_view v = n.attribute(attr).as_string();
    return v == "yes" || v == "1" || v == "true";
}

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
}

// Upstream setUserLexer: quoted keywords keep their inner spaces, encoded as
// '\v' ("...") or '\b' ('...') for LexUser; backslash escapes quotes.
std::string encodeKeywords(const std::string& kw)
{
    std::string out;
    bool inDouble = false, inSingle = false, nonWs = false;
    for (std::size_t j = 0, len = kw.size(); j < len; ++j) {
        const char c = kw[j];
        const char next = j + 1 < len ? kw[j + 1] : '\0';
        if (!inSingle && c == '"') {
            inDouble = !inDouble;
            continue;
        }
        if (!inDouble && c == '\'') {
            inSingle = !inSingle;
            continue;
        }
        if (c == '\\' && (next == '"' || next == '\'' || next == '\\')) {
            out += kw[++j];
            continue;
        }
        if (inDouble || inSingle) {
            if (static_cast<unsigned char>(c) > ' ') {
                out += c;
                nonWs = true;
            } else if (nonWs && j > 0 && kw[j - 1] != '"' && next != '"' && static_cast<unsigned char>(next) > ' ') {
                out += inDouble ? '\v' : '\b';
            }
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

bool UserLanguage::matchesExtension(std::string_view fileExt) const
{
    if (!fileExt.empty() && fileExt.front() == '.')
        fileExt.remove_prefix(1);
    if (fileExt.empty())
        return false;
    std::size_t p = 0;
    while (p < ext.size()) {
        const std::size_t e = ext.find(' ', p);
        const std::string_view tok = std::string_view(ext).substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (tok.size() == fileExt.size() && std::equal(tok.begin(), tok.end(), fileExt.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            return true;
        if (e == std::string::npos)
            break;
        p = e + 1;
    }
    return false;
}

std::vector<UserLanguage> loadUserLanguages(const fs::path& file, std::string* error)
{
    std::vector<UserLanguage> out;
    pugi::xml_document doc;
    if (const pugi::xml_parse_result res = doc.load_file(file.c_str()); !res) {
        if (error)
            *error = file.string() + ": " + res.description();
        return out;
    }
    for (const pugi::xml_node ul : doc.child("NotepadPlus").children("UserLang")) {
        UserLanguage l;
        l.name = ul.attribute("name").as_string();
        l.ext = ul.attribute("ext").as_string();
        l.udlVersion = ul.attribute("udlVersion").as_string();
        l.darkModeTheme = yes(ul, "darkModeTheme");
        l.file = file;
        const pugi::xml_node settings = ul.child("Settings"), lists = ul.child("KeywordLists"), styles = ul.child("Styles");
        if (l.name.empty() || !settings || !lists || !styles)
            continue; // upstream drops incomplete entries

        if (const pugi::xml_node g = settings.child("Global")) {
            l.caseIgnored = yes(g, "caseIgnored");
            l.allowFoldOfComments = yes(g, "allowFoldOfComments");
            l.foldCompact = yes(g, "foldCompact");
            l.forcePureLC = g.attribute("forcePureLC").as_int(0);
            l.decimalSeparator = g.attribute("decimalSeparator").as_int(0);
        }
        if (const pugi::xml_node prefix = settings.child("Prefix")) {
            if (l.udlVersion == "2.1" || l.udlVersion == "2.0") {
                for (int i = 0; i < SCE_USER_TOTAL_KEYWORD_GROUPS; ++i)
                    l.isPrefix[static_cast<std::size_t>(i)] = yes(prefix, ("Keywords" + std::to_string(i + 1)).c_str());
            } else {
                for (int i = 0; i < 4; ++i)
                    l.isPrefix[static_cast<std::size_t>(i)] = yes(prefix, ("words" + std::to_string(i + 1)).c_str());
            }
        }

        for (const pugi::xml_node k : lists.children("Keywords")) {
            const std::string kname = k.attribute("name").as_string();
            const std::string value = k.text().as_string();
            if (l.udlVersion.empty() && kname == "Delimiters" && value.size() >= 6) {
                // pre 2.0 delimiters: six characters, '0' = none
                std::string t = "00";
                if (value[0] != '0') t += value[0];
                t += " 01 02";
                if (value[3] != '0') t += value[3];
                t += " 03";
                if (value[1] != '0') t += value[1];
                t += " 04 05";
                if (value[4] != '0') t += value[4];
                t += " 06";
                if (value[2] != '0') t += value[2];
                t += " 07 08";
                if (value[5] != '0') t += value[5];
                t += " 09 10 11 12 13 14 15 16 17 18 19 20 21 22 23";
                l.keywordLists[SCE_USER_KWLIST_DELIMITERS] = t;
            } else if (kname == "Comment") { // pre 2.0
                std::string t = " " + value;
                replaceAll(t, " 0", " 00");
                replaceAll(t, " 1", " 03");
                replaceAll(t, " 2", " 04");
                t += " 01 02";
                if (!t.empty() && t[0] == ' ')
                    t.erase(0, 1);
                l.keywordLists[SCE_USER_KWLIST_COMMENTS] = t;
            } else if (auto it = keywordIds().find(kname); it != keywordIds().end()) {
                l.keywordLists[static_cast<std::size_t>(it->second)] = value;
            }
        }

        l.styles.resize(SCE_USER_STYLE_TOTAL_STYLES);
        l.nesting.assign(SCE_USER_STYLE_TOTAL_STYLES, 0);
        for (int i = 0; i < SCE_USER_STYLE_TOTAL_STYLES; ++i) {
            l.styles[static_cast<std::size_t>(i)].id = i;
            l.styles[static_cast<std::size_t>(i)].name = styleName(i);
        }
        for (const pugi::xml_node ws : styles.children("WordsStyle")) {
            auto it = styleIds().find(ws.attribute("name").as_string());
            if (it == styleIds().end())
                continue;
            Style& s = l.styles[static_cast<std::size_t>(it->second)];
            s.fg = StyleCatalog::parseColour(ws.attribute("fgColor").as_string());
            s.bg = StyleCatalog::parseColour(ws.attribute("bgColor").as_string());
            s.fontName = ws.attribute("fontName").as_string();
            if (const char* fs = ws.attribute("fontStyle").as_string(); *fs)
                s.fontStyle = std::atoi(fs);
            if (const char* sz = ws.attribute("fontSize").as_string(); *sz && std::atoi(sz) > 0)
                s.fontSize = std::atoi(sz);
            l.nesting[static_cast<std::size_t>(it->second)] = ws.attribute("nesting").as_int(0);
        }
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<UserLanguage> loadAllUserLanguages(const fs::path& configDir, const std::vector<fs::path>& dirs)
{
    std::vector<UserLanguage> all;
    std::set<std::string> names;
    auto add = [&](const fs::path& f) {
        std::string err;
        for (UserLanguage& l : loadUserLanguages(f, &err))
            if (names.insert(l.name).second)
                all.push_back(std::move(l));
    };
    std::error_code ec;
    if (fs::is_regular_file(configDir / "userDefineLang.xml", ec))
        add(configDir / "userDefineLang.xml");
    for (const fs::path& dir : dirs) {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".xml")
                files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files)
            add(f);
    }
    return all;
}

UserLexerSetup planUserLexer(const UserLanguage& lang, long udlId, long bufferId)
{
    UserLexerSetup s;
    auto prop = [&](std::string k, std::string v) { s.properties.emplace_back(std::move(k), std::move(v)); };
    prop("fold", "1");
    prop("userDefine.isCaseIgnored", lang.caseIgnored ? "1" : "0");
    prop("userDefine.allowFoldOfComments", lang.allowFoldOfComments ? "1" : "0");
    prop("userDefine.foldCompact", lang.foldCompact ? "1" : "0");
    for (int i = 0; i < SCE_USER_TOTAL_KEYWORD_GROUPS; ++i)
        prop("userDefine.prefixKeywords" + std::to_string(i + 1), lang.isPrefix[static_cast<std::size_t>(i)] ? "1" : "0");
    for (int i = 0; i < SCE_USER_KWLIST_TOTAL; ++i) {
        const std::string& kw = lang.keywordLists[static_cast<std::size_t>(i)];
        if (auto it = propertyLists().find(i); it != propertyLists().end())
            prop(it->second, kw);
        else // OPERATORS2, FOLDERS_IN_CODE2, FOLDERS_IN_COMMENT, KEYWORDS1-8
            s.keywordSets.push_back(encodeKeywords(kw));
    }
    prop("userDefine.forcePureLC", std::to_string(lang.forcePureLC));
    prop("userDefine.decimalSeparator", std::to_string(lang.decimalSeparator));
    prop("userDefine.udlName", std::to_string(udlId));
    prop("userDefine.currentBufferID", std::to_string(bufferId));
    for (std::size_t i = 0; i < lang.styles.size(); ++i) {
        char key[32];
        std::snprintf(key, sizeof key, "userDefine.nesting.%02d", static_cast<int>(i));
        prop(key, std::to_string(lang.nesting[i]));
        s.styles.push_back(lang.styles[i]);
    }
    return s;
}

} // namespace npp::core
