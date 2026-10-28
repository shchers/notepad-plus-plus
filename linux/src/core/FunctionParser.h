// Function List parsing — upstream WinControls/FunctionList/functionParser
// on core::Sci instead of ScintillaEditView. The rules are upstream's
// functionList/*.xml and overrideMap.xml, read unmodified; the regular
// expressions run through Scintilla's Boost engine with the same flags, so a
// document lists the same functions as on Windows.
#pragma once

#include "core/Sci.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace npp::core {

// Upstream foundInfo.
struct FunctionInfo {
    std::string name;       // _data: function name ("" when only a class was found)
    std::string className;  // _data2: enclosing class / struct / namespace, if any
    sptr_t pos = -1;        // where the name was found
    sptr_t classPos = -1;   // where the class name was found, -1 if inherited from a zone
    int level = 0;          // port: outline level (Markdown heading 1-6), 0 for none
};

class FunctionParser {
public:
    using Zones = std::vector<std::pair<sptr_t, sptr_t>>;
    FunctionParser(std::string id, std::string displayName, std::string commentExpr, std::string functionExpr,
                   std::vector<std::string> functionNameExprs, std::vector<std::string> classNameExprs)
        : id_(std::move(id)), displayName_(std::move(displayName)), commentExpr_(std::move(commentExpr)),
          functionExpr_(std::move(functionExpr)), functionNameExprs_(std::move(functionNameExprs)),
          classNameExprs_(std::move(classNameExprs))
    {
    }
    virtual ~FunctionParser() = default;
    virtual void parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className = {}) = 0;
    void funcParse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className = {},
                   const Zones* commentZones = nullptr);
    const std::string& id() const { return id_; }
    const std::string& displayName() const { return displayName_; }
    static bool isInZones(sptr_t pos, const Zones& zones);

protected:
    void commentZones(const Sci& sci, Zones& out, sptr_t begin, sptr_t end) const;
    static void invertZones(Zones& out, const Zones& in, sptr_t begin, sptr_t end);
    static std::string parseSubLevel(const Sci& sci, sptr_t begin, sptr_t end, std::vector<std::string> exprs, sptr_t& foundPos);

    std::string id_, displayName_, commentExpr_, functionExpr_;
    std::vector<std::string> functionNameExprs_, classNameExprs_;
};

// <classRange>: functions inside class bodies.
class FunctionZoneParser : public FunctionParser {
public:
    FunctionZoneParser(std::string id, std::string displayName, std::string commentExpr, std::string rangeExpr, std::string open,
                       std::string close, std::vector<std::string> classNameExprs, std::string functionExpr,
                       std::vector<std::string> functionNameExprs)
        : FunctionParser(std::move(id), std::move(displayName), std::move(commentExpr), std::move(functionExpr),
                         std::move(functionNameExprs), std::move(classNameExprs)),
          rangeExpr_(std::move(rangeExpr)), open_(std::move(open)), close_(std::move(close))
    {
    }
    void parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className = {}) override;

protected:
    void classParse(const Sci& sci, std::vector<FunctionInfo>& out, Zones& scanned, const Zones& comments, sptr_t begin, sptr_t end);

private:
    static sptr_t bodyClosePos(const Sci& sci, sptr_t begin, const std::string& open, const std::string& close, const Zones& comments);
    std::string rangeExpr_, open_, close_;
};

// <function>: free functions.
class FunctionUnitParser : public FunctionParser {
public:
    using FunctionParser::FunctionParser;
    void parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className = {}) override;
};

// Both: classes first, then free functions outside them.
class FunctionMixParser : public FunctionZoneParser {
public:
    FunctionMixParser(std::string id, std::string displayName, std::string commentExpr, std::string rangeExpr, std::string open,
                      std::string close, std::vector<std::string> classNameExprs, std::string functionExpr,
                      std::vector<std::string> functionNameExprs, std::unique_ptr<FunctionUnitParser> unit)
        : FunctionZoneParser(std::move(id), std::move(displayName), std::move(commentExpr), std::move(rangeExpr), std::move(open),
                             std::move(close), std::move(classNameExprs), std::move(functionExpr), std::move(functionNameExprs)),
          unit_(std::move(unit))
    {
    }
    void parse(const Sci& sci, std::vector<FunctionInfo>& out, sptr_t begin, sptr_t end, const std::string& className = {}) override;

private:
    std::unique_ptr<FunctionUnitParser> unit_;
};

// Upstream FunctionParsersManager: finds and loads the rules for a language.
class FunctionParsers {
public:
    // `userDir` first (the user's functionList/), then `installedDir`, as upstream.
    void init(const std::filesystem::path& userDir, const std::filesystem::path& installedDir);
    // `langName` is the langs.model.xml name ("cpp"); `udlName` a User Defined
    // Language name (then langName is ignored). Null when there are no rules.
    FunctionParser* parserFor(const std::string& langName, const std::string& udlName = {});
    // Parses the whole document. False when the language has no rules.
    bool parse(const Sci& sci, const std::string& langName, const std::string& udlName, std::vector<FunctionInfo>& out);

private:
    std::unique_ptr<FunctionParser> load(const std::filesystem::path& file) const;
    std::filesystem::path find(const std::string& fileName) const;
    std::filesystem::path userDir_, installedDir_;
    std::map<std::string, std::string> langFiles_; // langs.model.xml name -> rule file (overrideMap.xml)
    std::map<std::string, std::string> udlFiles_;  // UDL name -> rule file
    std::map<std::string, std::unique_ptr<FunctionParser>> cache_; // by file name; null = tried, none
};

} // namespace npp::core
