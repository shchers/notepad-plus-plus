#include "core/StyleCatalog.h"

#include "core/LanguageCatalog.h"

#include <pugixml.hpp>

#include <algorithm>
#include <charconv>

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

    pugi::xml_node root = doc.child("NotepadPlus");
    for (pugi::xml_node lt : root.child("LexerStyles").children("LexerType")) {
        auto& vec = lexers_[lt.attribute("name").as_string()];
        for (pugi::xml_node ws : lt.children("WordsStyle"))
            vec.push_back(parseStyle(ws));
    }
    for (pugi::xml_node ws : root.child("GlobalStyles").children("WidgetStyle"))
        globals_.push_back(parseStyle(ws));
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
