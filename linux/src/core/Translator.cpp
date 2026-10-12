#include "core/Translator.h"

#include <pugixml.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace npp::core {

std::string Translator::key(std::string_view a, std::string_view b)
{
    std::string k(a);
    k += '\x1f';
    k += b;
    return k;
}

std::string Translator::lookup(const std::string& k, std::string_view fallback) const
{
    if (auto it = strings_.find(k); it != strings_.end() && !it->second.empty())
        return it->second;
    return std::string(fallback);
}

bool Translator::merge(const fs::path& file, std::string* error)
{
    pugi::xml_document doc;
    if (const pugi::xml_parse_result res = doc.load_file(file.c_str()); !res) {
        if (error)
            *error = file.string() + ": " + res.description();
        return false;
    }
    const pugi::xml_node root = doc.child("NotepadPlus").child("Native-Langue");
    if (!root) {
        if (error)
            *error = file.string() + ": not a Notepad++ localization file";
        return false;
    }
    name_ = root.attribute("name").as_string("English");
    file_ = file.filename().string();
    rtl_ = std::string_view(root.attribute("RTL").as_string()) == "yes";

    const pugi::xml_node main = root.child("Menu").child("Main");
    for (const pugi::xml_node it : main.child("Entries").children("Item"))
        strings_[key("menu", it.attribute("menuId").as_string())] = it.attribute("name").as_string();
    for (const pugi::xml_node it : main.child("SubEntries").children("Item"))
        strings_[key("sub", it.attribute("subMenuId").as_string())] = it.attribute("name").as_string();
    for (const pugi::xml_node it : main.child("Commands").children("Item"))
        strings_[key("cmd", it.attribute("id").as_string())] = it.attribute("name").as_string();

    // Dialogs: every element with Item children, keyed by its path below <Dialog>.
    struct Walker {
        Translator& t;
        void walk(const pugi::xml_node& node, const std::string& path)
        {
            for (const pugi::xml_attribute a : node.attributes())
                t.strings_[key("dlgattr:" + path, a.name())] = a.value();
            for (const pugi::xml_node c : node.children()) {
                if (c.type() != pugi::node_element)
                    continue;
                if (std::string_view(c.name()) == "Item") {
                    if (c.attribute("id"))
                        t.strings_[key("dlg:" + path, c.attribute("id").as_string())] = c.attribute("name").as_string();
                } else {
                    walk(c, path + "/" + c.name());
                }
            }
        }
    };
    for (const pugi::xml_node d : root.child("Dialog").children())
        if (d.type() == pugi::node_element)
            Walker{*this}.walk(d, d.name());
    // Panels and other sections at the top level (<FolderAsWorkspace>,
    // <ProjectManager>, <DocList> ...), looked up the same way.
    for (const pugi::xml_node d : root.children()) {
        const std::string_view n = d.name();
        if (d.type() == pugi::node_element && n != "Menu" && n != "Dialog" && n != "MiscStrings" && n != "MessageBox")
            Walker{*this}.walk(d, d.name());
    }

    for (const pugi::xml_node m : root.child("MiscStrings").children())
        if (m.type() == pugi::node_element)
            strings_[key("misc", m.name())] = m.attribute("value").as_string();
    for (const pugi::xml_node m : root.child("MessageBox").children())
        if (m.type() == pugi::node_element)
            for (const pugi::xml_attribute a : m.attributes())
                strings_[key("msg:" + std::string(m.name()), a.name())] = a.value();
    return true;
}

bool Translator::load(const fs::path& dir, const std::string& fileName, std::string* error)
{
    strings_.clear();
    name_ = "English";
    file_ = "english.xml";
    rtl_ = false;
    std::error_code ec;
    if (fs::is_regular_file(dir / "english.xml", ec))
        merge(dir / "english.xml", nullptr);
    if (fileName.empty() || fileName == "english.xml")
        return true;
    // Only plain file names: a localization never points outside its folder.
    if (fs::path(fileName).filename().string() != fileName) {
        if (error)
            *error = "invalid localization file name: " + fileName;
        return false;
    }
    return merge(dir / fileName, error);
}

std::vector<Translator::Language> Translator::available(const fs::path& dir)
{
    std::vector<Language> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".xml")
            continue;
        // The language name sits at the top: read the header, not the file.
        std::ifstream in(e.path(), std::ios::binary);
        std::string head(8192, '\0');
        in.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(in.gcount()));
        const std::size_t tag = head.find("<Native-Langue");
        const std::size_t close = tag == std::string::npos ? tag : head.find('>', tag);
        if (close == std::string::npos)
            continue;
        pugi::xml_document doc;
        if (!doc.load_string((head.substr(tag, close - tag) + "/>").c_str()))
            continue;
        const pugi::xml_node n = doc.child("Native-Langue");
        if (n && *n.attribute("name").as_string())
            out.push_back({n.attribute("name").as_string(), e.path().filename().string()});
    }
    std::sort(out.begin(), out.end(), [](const Language& a, const Language& b) { return a.name < b.name; });
    return out;
}

std::string Translator::menu(std::string_view menuId, std::string_view fallback) const
{
    return lookup(key("menu", menuId), fallback);
}

std::string Translator::subMenu(std::string_view subMenuId, std::string_view fallback) const
{
    return lookup(key("sub", subMenuId), fallback);
}

std::string Translator::command(int id, std::string_view fallback) const
{
    return lookup(key("cmd", std::to_string(id)), fallback);
}

std::string Translator::dialog(std::string_view path, int id, std::string_view fallback) const
{
    return lookup(key("dlg:" + std::string(path), std::to_string(id)), fallback);
}

std::string Translator::dialogAttr(std::string_view path, std::string_view attr, std::string_view fallback) const
{
    return lookup(key("dlgattr:" + std::string(path), attr), fallback);
}

std::string Translator::misc(std::string_view k, std::string_view fallback) const
{
    return lookup(key("misc", k), fallback);
}

std::string Translator::message(std::string_view box, std::string_view attr, std::string_view fallback) const
{
    return lookup(key("msg:" + std::string(box), attr), fallback);
}

std::string Translator::toMnemonic(std::string_view label)
{
    if (const std::size_t tab = label.find('\t'); tab != std::string_view::npos)
        label = label.substr(0, tab);
    std::string out;
    for (std::size_t i = 0; i < label.size(); ++i) {
        const char c = label[i];
        if (c == '&') {
            if (i + 1 < label.size() && label[i + 1] == '&') {
                out += '&';
                ++i;
            } else {
                out += '_';
            }
        } else if (c == '_') {
            out += "__";
        } else {
            out += c;
        }
    }
    return out;
}

std::string Translator::plain(std::string_view label)
{
    if (const std::size_t tab = label.find('\t'); tab != std::string_view::npos)
        label = label.substr(0, tab);
    std::string out;
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '&') {
            if (i + 1 < label.size() && label[i + 1] == '&')
                out += '&', ++i;
            continue;
        }
        out += label[i];
    }
    return out;
}

std::string Translator::format(std::string text, std::string_view placeholder, std::string_view value)
{
    for (std::size_t p = text.find(placeholder); p != std::string::npos; p = text.find(placeholder, p + value.size()))
        text.replace(p, placeholder.size(), value);
    return text;
}

} // namespace npp::core
