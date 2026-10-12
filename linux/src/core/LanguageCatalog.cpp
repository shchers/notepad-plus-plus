#include "core/LanguageCatalog.h"

#include "LangTable.generated.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace npp::core {

namespace {

std::string lower(std::string_view s)
{
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return std::tolower(c); });
    return r;
}

// Upstream special-cases these file names (no usable extension).
const std::unordered_map<std::string, std::string> kFileNameLangs = {
    {"makefile", "makefile"},
    {"gnumakefile", "makefile"},
    {"cmakelists.txt", "cmake"},
};

} // namespace

int LanguageCatalog::keywordClassIndex(std::string_view c)
{
    static const std::unordered_map<std::string_view, int> map = {
        {"instre1", 0}, {"instre2", 1}, {"type1", 2}, {"type2", 3}, {"type3", 4},
        {"type4", 5},   {"type5", 6},   {"type6", 7}, {"type7", 8},
    };
    if (auto it = map.find(c); it != map.end())
        return it->second;
    if (c.size() == 1 && c[0] >= '0' && c[0] <= '8')
        return c[0] - '0';
    return -1;
}

bool LanguageCatalog::load(const std::filesystem::path& langsXml, std::string* error)
{
    pugi::xml_document doc;
    pugi::xml_parse_result res = doc.load_file(langsXml.c_str());
    if (!res) {
        if (error)
            *error = langsXml.string() + ": " + res.description();
        return false;
    }

    std::unordered_map<std::string, pugi::xml_node> xmlLangs;
    for (pugi::xml_node n : doc.child("NotepadPlus").child("Languages").children("Language"))
        xmlLangs.emplace(n.attribute("name").as_string(), n);

    languages_.clear();
    for (const auto& row : gen::kLangTable) {
        std::string_view lexer = row.lexer;
        if (lexer == "user")
            continue; // User Defined Languages: future work, see PORTING.md
        auto it = xmlLangs.find(row.name);
        const bool plain = std::string_view(row.name) == "normal";
        if (it == xmlLangs.end() && !plain)
            continue; // e.g. "ext" placeholder, or a language upstream ships without a model entry

        Language lang;
        lang.name = row.name;
        lang.shortName = row.shortName;
        lang.longName = row.longName;
        lang.lexer = row.lexer;
        if (it != xmlLangs.end()) {
            pugi::xml_node n = it->second;
            std::istringstream exts(n.attribute("ext").as_string());
            for (std::string e; exts >> e;)
                lang.extensions.push_back(lower(e));
            lang.commentLine = n.attribute("commentLine").as_string();
            lang.commentStart = n.attribute("commentStart").as_string();
            lang.commentEnd = n.attribute("commentEnd").as_string();
            for (pugi::xml_node kw : n.children("Keywords")) {
                int idx = keywordClassIndex(kw.attribute("name").as_string());
                if (idx >= 0)
                    lang.keywords[idx] = kw.text().as_string();
            }
        }
        languages_.push_back(std::move(lang));
    }

    if (!byName("normal")) {
        Language plain{"normal", "Normal text", "Normal text file", "null", {"txt"}, {}, {}, {}, {}};
        languages_.insert(languages_.begin(), std::move(plain));
    }
    return true;
}

const Language* LanguageCatalog::byName(std::string_view name) const
{
    auto it = std::find_if(languages_.begin(), languages_.end(), [&](const Language& l) { return l.name == name; });
    return it == languages_.end() ? nullptr : &*it;
}

const Language* LanguageCatalog::byAnyName(std::string_view name) const
{
    const std::string key = lower(name);
    for (const auto& l : languages_)
        if (lower(l.name) == key || lower(l.shortName) == key)
            return &l;
    return nullptr;
}

const Language& LanguageCatalog::plainText() const
{
    // load() guarantees "normal" exists; before load() callers get a static.
    if (const Language* l = byName("normal"))
        return *l;
    static const Language fallback{"normal", "Normal text", "Normal text file", "null", {}, {}, {}, {}, {}};
    return fallback;
}

const Language& LanguageCatalog::forFile(const std::filesystem::path& file) const
{
    const std::string fname = lower(file.filename().string());
    if (auto it = kFileNameLangs.find(fname); it != kFileNameLangs.end())
        if (const Language* l = byName(it->second))
            return *l;

    // ".bashrc" has no stem in std::filesystem terms; Notepad++ treats the
    // text after the last dot as the extension in both cases.
    const auto dot = fname.rfind('.');
    if (dot == std::string::npos)
        return plainText();
    const std::string ext = fname.substr(dot + 1);
    if (ext.empty())
        return plainText();

    for (const auto& l : languages_) {
        if (l.isPlainText())
            continue;
        if (std::find(l.extensions.begin(), l.extensions.end(), ext) != l.extensions.end())
            return l;
    }
    return plainText();
}

} // namespace npp::core
