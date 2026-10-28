#include "core/FunctionParser.h"

#include "LangTable.generated.h"

#include <BoostRegexSearch.h> // SCFIND_REGEXP_DOTMATCHESNL

#include <pugixml.hpp>

#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

constexpr int kFlags = SCFIND_REGEXP | SCFIND_POSIX | SCFIND_REGEXP_DOTMATCHESNL;

sptr_t searchInTarget(const Sci& sci, const std::string& expr, sptr_t from, sptr_t to)
{
    sci.setTarget(from, to);
    return sci(SCI_SEARCHINTARGET, expr.size(), reinterpret_cast<sptr_t>(expr.data()));
}

// Upstream copies found text into a 1024-character buffer.
std::string foundText(const Sci& sci, sptr_t from, sptr_t to)
{
    return sci.range(from, std::min(to, from + 1023));
}

std::vector<std::string> exprList(const pugi::xml_node& parent, const char* child, const char* item)
{
    std::vector<std::string> out;
    if (const pugi::xml_node n = parent.child(child))
        for (const pugi::xml_node e : n.children(item))
            if (const char* expr = e.attribute("expr").as_string(); *expr)
                out.emplace_back(expr);
    return out;
}

} // namespace

// ============================================================================
// FunctionParser
// ============================================================================

bool FunctionParser::isInZones(sptr_t pos, const Zones& zones)
{
    for (const auto& [a, b] : zones)
        if (pos >= a && pos < b)
            return true;
    return false;
}

void FunctionParser::funcParse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className,
                               const Zones* commentZones)
{
    if (begin >= end || functionExpr_.empty())
        return;
    sci(SCI_SETSEARCHFLAGS, kFlags);
    sptr_t start = searchInTarget(sci, functionExpr_, begin, end);
    while (start >= 0) {
        start = sci(SCI_GETTARGETSTART);
        const sptr_t tEnd = sci(SCI_GETTARGETEND);
        if (tEnd > end) // a result outside the range
            break;
        if (tEnd == end)
            break;
        FunctionInfo fi;
        if (functionNameExprs_.empty() && classNameExprs_.empty()) {
            fi.name = foundText(sci, start, tEnd);
            fi.pos = start;
        } else {
            sptr_t found = -1;
            if (!functionNameExprs_.empty()) {
                fi.name = parseSubLevel(sci, start, tEnd, functionNameExprs_, found);
                fi.pos = found;
            }
            if (!className.empty()) {
                fi.className = className;
                fi.classPos = -1;
            } else if (!classNameExprs_.empty()) {
                fi.className = parseSubLevel(sci, start, tEnd, classNameExprs_, found);
                fi.classPos = found;
            }
        }
        if (fi.pos != -1 || fi.classPos != -1) {
            if (!commentZones || (!isInZones(fi.pos, *commentZones) && !isInZones(fi.classPos, *commentZones)))
                out.push_back(std::move(fi));
        }
        begin = tEnd > start ? tEnd : tEnd + 1; // an empty match would loop
        sci(SCI_SETSEARCHFLAGS, kFlags);
        start = searchInTarget(sci, functionExpr_, begin, end);
    }
}

std::string FunctionParser::parseSubLevel(const Sci& sci, sptr_t begin, sptr_t end, std::vector<std::string> exprs, sptr_t& foundPos)
{
    if (begin >= end) {
        foundPos = -1;
        return {};
    }
    if (exprs.empty())
        return {};
    sci(SCI_SETSEARCHFLAGS, kFlags);
    const sptr_t start = searchInTarget(sci, exprs[0], begin, end);
    if (start < 0) {
        foundPos = -1;
        return {};
    }
    const sptr_t tEnd = sci(SCI_GETTARGETEND);
    if (exprs.size() >= 2) {
        exprs.erase(exprs.begin());
        return parseSubLevel(sci, start, tEnd, std::move(exprs), foundPos);
    }
    foundPos = start;
    return foundText(sci, start, tEnd);
}

void FunctionParser::commentZones(const Sci& sci, Zones& out, sptr_t begin, sptr_t end) const
{
    if (begin >= end || commentExpr_.empty())
        return;
    sci(SCI_SETSEARCHFLAGS, kFlags);
    sptr_t start = searchInTarget(sci, commentExpr_, begin, end);
    while (start >= 0) {
        start = sci(SCI_GETTARGETSTART);
        const sptr_t tEnd = sci(SCI_GETTARGETEND);
        if (tEnd > end)
            break;
        out.emplace_back(start, tEnd);
        if (tEnd == end || tEnd == start) // an empty match would loop
            break;
        start = searchInTarget(sci, commentExpr_, tEnd, end);
    }
}

void FunctionParser::invertZones(Zones& out, const Zones& in, sptr_t begin, sptr_t end)
{
    // Upstream getInvertZones, with its off-by-one gaps kept for identical results.
    if (in.empty()) {
        out.emplace_back(begin, end);
        return;
    }
    if (begin < in[0].first)
        out.emplace_back(begin, in[0].first - 1);
    std::size_t i = 0;
    for (; i + 1 < in.size(); ++i) {
        const sptr_t b = in[i].second + 1, e = in[i + 1].first - 1;
        if (b < e)
            out.emplace_back(b, e);
    }
    const sptr_t last = in[i].second + 1;
    if (last < end)
        out.emplace_back(last, end);
}

// ============================================================================
// Zone / unit / mix parsers
// ============================================================================

sptr_t FunctionZoneParser::bodyClosePos(const Sci& sci, sptr_t begin, const std::string& open, const std::string& close, const Zones& comments)
{
    std::size_t depth = 1;
    const sptr_t len = sci.length();
    if (begin >= len)
        return len;
    const std::string expr = "(" + open + "|" + close + ")";
    sci(SCI_SETSEARCHFLAGS, kFlags);
    sptr_t start = searchInTarget(sci, expr, begin, len);
    sptr_t tEnd = 0;
    do {
        if (start >= 0) {
            tEnd = sci(SCI_GETTARGETEND);
            if (!isInZones(start, comments)) {
                if (searchInTarget(sci, open, start, tEnd) >= 0)
                    ++depth;
                else
                    --depth;
            }
        } else {
            depth = 0;
            tEnd = begin;
        }
        start = searchInTarget(sci, expr, tEnd, len);
    } while (depth);
    return tEnd;
}

void FunctionZoneParser::classParse(const Sci& sci, std::vector<FunctionInfo>& out, Zones& scanned, const Zones& comments, sptr_t begin, sptr_t end)
{
    if (begin >= end)
        return;
    sci(SCI_SETSEARCHFLAGS, kFlags);
    sptr_t start = searchInTarget(sci, rangeExpr_, begin, end);
    while (start >= 0) {
        sptr_t tEnd = sci(SCI_GETTARGETEND);
        sptr_t found = 0;
        const std::string cls = parseSubLevel(sci, start, tEnd, classNameExprs_, found);
        if (!open_.empty() && !close_.empty())
            tEnd = bodyClosePos(sci, tEnd, open_, close_, comments);
        if (tEnd > end)
            break;
        scanned.emplace_back(start, tEnd);
        if (tEnd == end)
            break;
        if (!isInZones(start, comments))
            funcParse(sci, out, start, tEnd, cls, &comments);
        if (tEnd <= start) // an empty match would loop
            break;
        begin = tEnd;
        sci(SCI_SETSEARCHFLAGS, kFlags);
        start = searchInTarget(sci, rangeExpr_, begin, end);
    }
}

void FunctionZoneParser::parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string&)
{
    Zones classZones, comments, nonComments;
    commentZones(sci, comments, begin, end);
    invertZones(nonComments, comments, begin, end);
    for (const auto& [a, b] : nonComments)
        classParse(sci, out, classZones, comments, a, b);
}

void FunctionUnitParser::parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className)
{
    Zones comments, nonComments;
    commentZones(sci, comments, begin, end);
    invertZones(nonComments, comments, begin, end);
    for (const auto& [a, b] : nonComments)
        funcParse(sci, out, a, b, className);
}

void FunctionMixParser::parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string&)
{
    Zones comments, scanned, nonScanned;
    commentZones(sci, comments, begin, end);
    classParse(sci, out, scanned, comments, begin, end);
    // the second level, as upstream
    for (const auto& [a, b] : Zones(scanned)) {
        Zones tmp;
        classParse(sci, out, tmp, comments, a, b);
    }
    invertZones(nonScanned, scanned, begin, end);
    if (unit_)
        for (const auto& [a, b] : nonScanned)
            unit_->funcParse(sci, out, a, b, {}, &comments);
}

// ============================================================================
// FunctionParsers
// ============================================================================

void FunctionParsers::init(const fs::path& userDir, const fs::path& installedDir)
{
    userDir_ = userDir;
    installedDir_ = installedDir;
    langFiles_.clear();
    udlFiles_.clear();
    cache_.clear();
    // overrideMap.xml: the user's, else the installed one (upstream init).
    for (const fs::path& dir : {userDir, installedDir}) {
        pugi::xml_document doc;
        if (!doc.load_file((dir / "overrideMap.xml").c_str()))
            continue;
        const pugi::xml_node map = doc.child("NotepadPlus").child("functionList").child("associationMap");
        for (const pugi::xml_node a : map.children("association")) {
            const std::string id = a.attribute("id").as_string();
            if (id.empty())
                continue;
            const int langId = a.attribute("langID").as_int(-1);
            const std::string udl = a.attribute("userDefinedLangName").as_string();
            if (langId >= 0 && langId < static_cast<int>(std::size(gen::kLangTable)))
                langFiles_[gen::kLangTable[langId].name] = id;
            else if (!udl.empty())
                udlFiles_[udl] = id;
        }
        break;
    }
}

fs::path FunctionParsers::find(const std::string& fileName) const
{
    std::error_code ec;
    for (const fs::path& dir : {userDir_, installedDir_})
        if (!dir.empty() && fs::is_regular_file(dir / fileName, ec))
            return dir / fileName;
    return {};
}

std::unique_ptr<FunctionParser> FunctionParsers::load(const fs::path& file) const
{
    pugi::xml_document doc;
    if (!doc.load_file(file.c_str(), pugi::parse_cdata | pugi::parse_escapes | pugi::parse_eol))
        return nullptr;
    const pugi::xml_node p = doc.child("NotepadPlus").child("functionList").child("parser");
    const std::string id = p.attribute("id").as_string();
    if (id.empty())
        return nullptr;
    const std::string comment = p.attribute("commentExpr").as_string();
    std::string display = p.attribute("displayName").as_string();
    if (display.empty())
        display = id;
    const pugi::xml_node range = p.child("classRange"), func = p.child("function");

    auto zoneParams = [](const pugi::xml_node& r, std::string& main, std::string& open, std::string& close, std::vector<std::string>& classNames,
                         std::string& funcExpr, std::vector<std::string>& funcNames) {
        main = r.attribute("mainExpr").as_string();
        open = r.attribute("openSymbole").as_string();
        close = r.attribute("closeSymbole").as_string();
        classNames = exprList(r, "className", "nameExpr");
        const pugi::xml_node f = r.child("function");
        funcExpr = f.attribute("mainExpr").as_string();
        funcNames = exprList(f, "functionName", "funcNameExpr");
    };
    if (range && func) {
        std::string main, open, close, funcExpr;
        std::vector<std::string> classNames, funcNames;
        zoneParams(range, main, open, close, classNames, funcExpr, funcNames);
        auto unit = std::make_unique<FunctionUnitParser>(id, display, comment, func.attribute("mainExpr").as_string(),
                                                         exprList(func, "functionName", "nameExpr"), exprList(func, "className", "nameExpr"));
        return std::make_unique<FunctionMixParser>(id, display, comment, main, open, close, classNames, funcExpr, funcNames, std::move(unit));
    }
    if (range) {
        std::string main, open, close, funcExpr;
        std::vector<std::string> classNames, funcNames;
        zoneParams(range, main, open, close, classNames, funcExpr, funcNames);
        return std::make_unique<FunctionZoneParser>(id, display, comment, main, open, close, classNames, funcExpr, funcNames);
    }
    if (func)
        return std::make_unique<FunctionUnitParser>(id, display, comment, func.attribute("mainExpr").as_string(),
                                                    exprList(func, "functionName", "nameExpr"), exprList(func, "className", "nameExpr"));
    return nullptr;
}

FunctionParser* FunctionParsers::parserFor(const std::string& langName, const std::string& udlName)
{
    std::string file;
    if (!udlName.empty()) {
        auto it = udlFiles_.find(udlName);
        if (it == udlFiles_.end())
            return nullptr;
        file = it->second;
    } else {
        // Upstream FunctionListPanel::reload: embedded JavaScript uses the
        // JavaScript rules.
        const std::string lang = langName == "javascript" ? "javascript.js" : langName;
        auto it = langFiles_.find(lang);
        file = it != langFiles_.end() ? it->second : lang + ".xml";
    }
    auto [it, fresh] = cache_.try_emplace(file);
    if (fresh)
        if (const fs::path p = find(file); !p.empty())
            it->second = load(p);
    return it->second.get();
}

namespace {

// The level of the Markdown heading whose name is at `pos`: the number of
// '#'s, or for a Setext heading 1 when underlined with '=', 2 with '-'.
int markdownHeadingLevel(const Sci& sci, sptr_t pos)
{
    const sptr_t line = sci.lineOf(pos);
    const std::string text = sci.lineText(line);
    std::size_t i = text.find_first_not_of(" \t");
    if (i != std::string::npos && text[i] == '#') {
        int level = 0;
        while (i < text.size() && text[i] == '#')
            ++level, ++i;
        return std::min(level, 6);
    }
    const std::string next = sci.lineText(line + 1);
    const std::size_t j = next.find_first_not_of(" \t");
    return j != std::string::npos && next[j] == '=' ? 1 : 2;
}

} // namespace

bool FunctionParsers::parse(const Sci& sci, const std::string& langName, const std::string& udlName, std::vector<FunctionInfo>& out)
{
    FunctionParser* p = parserFor(langName, udlName);
    if (!p)
        return false;
    p->parse(sci, out, 0, sci.length());
    // Port: Markdown headings nest by level in the panel (upstream lists them flat).
    if (p->id() == "markdown_heading")
        for (FunctionInfo& fi : out)
            fi.level = markdownHeadingLevel(sci, fi.pos);
    return true;
}

} // namespace npp::core
