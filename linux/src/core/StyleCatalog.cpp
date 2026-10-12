#include "core/StyleCatalog.h"

#include "core/LanguageCatalog.h"

#include <pugixml.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>

namespace npp::core {

namespace {

std::optional<int> parseInt(const char* s)
{
    if (!s || !*s)
        return std::nullopt;
    int v = 0;
    const char* end = s + std::char_traits<char>::length(s);
    auto [p, ec] = std::from_chars(s, end, v);
    if (ec != std::errc() || p != end)
        return std::nullopt;
    return v;
}

Style parseStyle(const pugi::xml_node& n)
{
    Style s;
    s.name = n.attribute("name").as_string();
    s.id = n.attribute("styleID").as_int();
    s.fg = StyleCatalog::parseColour(n.attribute("fgColor").as_string());
    s.bg = StyleCatalog::parseColour(n.attribute("bgColor").as_string());
    s.fontName = n.attribute("fontName").as_string();
    s.fontStyle = parseInt(n.attribute("fontStyle").as_string());
    s.fontSize = parseInt(n.attribute("fontSize").as_string());
    s.keywordClass = LanguageCatalog::keywordClassIndex(n.attribute("keywordClass").as_string());
    s.userKeywords = n.text().as_string();
    return s;
}

} // namespace

std::optional<std::uint32_t> StyleCatalog::parseColour(std::string_view hex)
{
    if (hex.size() != 6)
        return std::nullopt;
    std::uint32_t rgb = 0;
    auto [p, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), rgb, 16);
    if (ec != std::errc() || p != hex.data() + hex.size())
        return std::nullopt;
    const std::uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return r | (g << 8) | (b << 16);
}

bool StyleCatalog::load(const std::filesystem::path& stylersXml, std::string* error)
{
    pugi::xml_document doc;
    pugi::xml_parse_result res = doc.load_file(stylersXml.c_str());
    if (!res) {
        if (error)
            *error = stylersXml.string() + ": " + res.description();
        return false;
    }
    lexers_.clear();
    globals_.clear();
    lexerInfo_.clear();
    source_ = stylersXml;

    pugi::xml_node root = doc.child("NotepadPlus");
    for (pugi::xml_node lt : root.child("LexerStyles").children("LexerType")) {
        lexerInfo_.push_back({lt.attribute("name").as_string(), lt.attribute("desc").as_string(), lt.attribute("ext").as_string()});
        auto& vec = lexers_[lt.attribute("name").as_string()];
        for (pugi::xml_node ws : lt.children("WordsStyle"))
            vec.push_back(parseStyle(ws));
    }
    for (pugi::xml_node ws : root.child("GlobalStyles").children("WidgetStyle"))
        globals_.push_back(parseStyle(ws));
    return true;
}

std::string StyleCatalog::toHex(std::uint32_t bgr)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X", bgr & 0xFF, (bgr >> 8) & 0xFF, (bgr >> 16) & 0xFF);
    return buf;
}

std::vector<Style>* StyleCatalog::mutableLexer(std::string_view name)
{
    auto it = lexers_.find(std::string(name));
    return it == lexers_.end() ? nullptr : &it->second;
}

void StyleCatalog::setLexerExt(std::string_view name, std::string ext)
{
    for (LexerInfo& l : lexerInfo_)
        if (l.name == name)
            l.ext = std::move(ext);
}

namespace {

void writeStyle(pugi::xml_node n, const Style& s)
{
    auto set = [&](const char* attr, const std::string& v) {
        pugi::xml_attribute a = n.attribute(attr);
        if (!a)
            a = n.append_attribute(attr);
        a.set_value(v.c_str());
    };
    // Colours the theme leaves unset stay unset.
    if (s.fg || n.attribute("fgColor"))
        set("fgColor", s.fg ? StyleCatalog::toHex(*s.fg) : "");
    if (s.bg || n.attribute("bgColor"))
        set("bgColor", s.bg ? StyleCatalog::toHex(*s.bg) : "");
    set("fontName", s.fontName);
    set("fontStyle", s.fontStyle ? std::to_string(*s.fontStyle) : "");
    set("fontSize", s.fontSize ? std::to_string(*s.fontSize) : "");
    if (s.keywordClass >= 0 || !s.userKeywords.empty())
        n.text().set(s.userKeywords.c_str());
}

} // namespace

bool StyleCatalog::save(const std::filesystem::path& target, std::string* error) const
{
    pugi::xml_document doc;
    if (const pugi::xml_parse_result res = doc.load_file(source_.c_str()); !res) {
        if (error)
            *error = source_.string() + ": " + res.description();
        return false;
    }
    pugi::xml_node root = doc.child("NotepadPlus");
    for (pugi::xml_node lt : root.child("LexerStyles").children("LexerType")) {
        const std::string name = lt.attribute("name").as_string();
        for (const LexerInfo& l : lexerInfo_)
            if (l.name == name) {
                pugi::xml_attribute a = lt.attribute("ext");
                (a ? a : lt.append_attribute("ext")).set_value(l.ext.c_str());
            }
        auto it = lexers_.find(name);
        if (it == lexers_.end())
            continue;
        std::size_t i = 0;
        for (pugi::xml_node ws : lt.children("WordsStyle"))
            if (i < it->second.size())
                writeStyle(ws, it->second[i++]);
    }
    std::size_t i = 0;
    for (pugi::xml_node ws : root.child("GlobalStyles").children("WidgetStyle"))
        if (i < globals_.size())
            writeStyle(ws, globals_[i++]);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (!doc.save_file(target.c_str(), "    ", pugi::format_default, pugi::encoding_utf8)) {
        if (error)
            *error = "Cannot write " + target.string();
        return false;
    }
    return true;
}

const std::vector<Style>* StyleCatalog::forLanguage(std::string_view langName) const
{
    auto it = lexers_.find(std::string(langName));
    return it == lexers_.end() ? nullptr : &it->second;
}

const Style* StyleCatalog::global(std::string_view name) const
{
    auto it = std::find_if(globals_.begin(), globals_.end(), [&](const Style& s) { return s.name == name; });
    return it == globals_.end() ? nullptr : &*it;
}

const Style* StyleCatalog::globalById(int id) const
{
    auto it = std::find_if(globals_.begin(), globals_.end(), [&](const Style& s) { return s.id == id; });
    return it == globals_.end() ? nullptr : &*it;
}

} // namespace npp::core
