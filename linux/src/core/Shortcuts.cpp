#include "core/Shortcuts.h"

#include "CommandTables.generated.h"
#include "platform/FileIO.h"

#include <pugixml.hpp>

#include <sstream>

namespace npp::core {

namespace {

KeyCombo readKey(const pugi::xml_node& n)
{
    auto yes = [&](const char* a) { return std::string_view(n.attribute(a).as_string()) == "yes"; };
    return {yes("Ctrl"), yes("Alt"), yes("Shift"), n.attribute("Key").as_int(0)};
}

void writeKey(pugi::xml_node n, const KeyCombo& k)
{
    n.append_attribute("Ctrl") = k.ctrl ? "yes" : "no";
    n.append_attribute("Alt") = k.alt ? "yes" : "no";
    n.append_attribute("Shift") = k.shift ? "yes" : "no";
    n.append_attribute("Key") = k.vk;
}

std::string serialise(const pugi::xml_node& n)
{
    if (!n)
        return {};
    std::ostringstream os;
    n.print(os, "    ", pugi::format_default | pugi::format_no_declaration);
    return os.str();
}

} // namespace

bool ShortcutsFile::load(const std::filesystem::path& file, std::string* error)
{
    *this = ShortcutsFile{};
    pugi::xml_document doc;
    if (const pugi::xml_parse_result res = doc.load_file(file.c_str()); !res) {
        if (error)
            *error = file.string() + ": " + res.description();
        return false;
    }
    const pugi::xml_node root = doc.child("NotepadPlus");
    if (!root) {
        if (error)
            *error = file.string() + ": not a Notepad++ shortcuts file";
        return false;
    }
    for (const pugi::xml_node s : root.child("InternalCommands").children("Shortcut"))
        if (const int id = s.attribute("id").as_int(-1); id > 0)
            commands.push_back({id, s.attribute("nth").as_int(0), readKey(s)});
    for (const pugi::xml_node m : root.child("Macros").children("Macro")) {
        SavedMacro sm{m.attribute("name").as_string(), readKey(m), m.attribute("FolderName").as_string(), {}};
        if (sm.name.empty())
            continue;
        for (const pugi::xml_node a : m.children("Action")) {
            const int type = a.attribute("type").as_int(4);
            if (type > MacroStep::SavedSearch)
                continue;
            sm.steps.push_back({type, a.attribute("message").as_int(0), a.attribute("wParam").as_llong(0),
                                a.attribute("lParam").as_llong(0), a.attribute("sParam").as_string()});
        }
        sm.steps = normaliseNewlines(sm.steps);
        macros.push_back(std::move(sm));
    }
    for (const pugi::xml_node c : root.child("UserDefinedCommands").children("Command")) {
        UserCommand uc{c.attribute("name").as_string(), readKey(c), c.attribute("FolderName").as_string(), c.text().as_string()};
        if (!uc.name.empty() && !uc.command.empty())
            userCommands.push_back(std::move(uc));
    }
    pluginCommandsXml_ = serialise(root.child("PluginCommands"));
    scintillaKeysXml_ = serialise(root.child("ScintillaKeys"));
    return true;
}

bool ShortcutsFile::save(const std::filesystem::path& file, std::string* error) const
{
    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";
    pugi::xml_node root = doc.append_child("NotepadPlus");

    pugi::xml_node cmds = root.append_child("InternalCommands");
    for (const CommandKey& c : commands) {
        pugi::xml_node s = cmds.append_child("Shortcut");
        s.append_attribute("id") = c.id;
        writeKey(s, c.key);
        if (c.nth)
            s.append_attribute("nth") = c.nth;
    }
    pugi::xml_node macrosNode = root.append_child("Macros");
    for (const SavedMacro& m : macros) {
        pugi::xml_node n = macrosNode.append_child("Macro");
        n.append_attribute("name") = m.name.c_str();
        writeKey(n, m.key);
        if (!m.folder.empty())
            n.append_attribute("FolderName") = m.folder.c_str();
        for (const MacroStep& s : m.steps) {
            pugi::xml_node a = n.append_child("Action");
            a.append_attribute("type") = s.type;
            a.append_attribute("message") = s.message;
            a.append_attribute("wParam") = static_cast<long long>(s.wParam);
            a.append_attribute("lParam") = static_cast<long long>(s.lParam);
            a.append_attribute("sParam") = s.sParam.c_str();
        }
    }
    pugi::xml_node userNode = root.append_child("UserDefinedCommands");
    for (const UserCommand& c : userCommands) {
        pugi::xml_node n = userNode.append_child("Command");
        n.append_attribute("name") = c.name.c_str();
        writeKey(n, c.key);
        if (!c.folder.empty())
            n.append_attribute("FolderName") = c.folder.c_str();
        n.text().set(c.command.c_str());
    }
    for (const std::string* kept : {&pluginCommandsXml_, &scintillaKeysXml_}) {
        pugi::xml_document frag;
        if (!kept->empty() && frag.load_string(kept->c_str()))
            root.append_copy(frag.first_child());
    }
    if (!root.child("PluginCommands"))
        root.append_child("PluginCommands");
    if (!root.child("ScintillaKeys"))
        root.append_child("ScintillaKeys");

    std::ostringstream os;
    doc.save(os, "    ", pugi::format_default, pugi::encoding_utf8);
    return platform::writeFileAtomic(file, os.str(), error);
}

// ---------------------------------------------------------------------------

CommandKeys::CommandKeys()
{
    for (const gen::DefaultKey& d : gen::kDefaultKeys) {
        int nth = 0;
        for (const CommandKey& k : defaults_)
            if (k.id == d.id)
                ++nth;
        defaults_.push_back({d.id, nth, {d.ctrl, d.alt, d.shift, d.vk}});
    }
    keys_ = defaults_;
}

const char* CommandKeys::defaultName(int id)
{
    for (const gen::DefaultKey& d : gen::kDefaultKeys)
        if (d.id == id && d.name)
            return d.name;
    return nullptr;
}

std::vector<KeyCombo> CommandKeys::keysOf(int id) const
{
    std::vector<KeyCombo> out;
    for (const CommandKey& k : keys_)
        if (k.id == id && !k.key.empty())
            out.push_back(k.key);
    return out;
}

void CommandKeys::set(int id, int nth, const KeyCombo& key)
{
    for (CommandKey& k : keys_)
        if (k.id == id && k.nth == nth) {
            k.key = key;
            return;
        }
    keys_.push_back({id, nth, key}); // a command without a default row
}

void CommandKeys::apply(const std::vector<CommandKey>& changes)
{
    keys_ = defaults_;
    for (const CommandKey& c : changes)
        set(c.id, c.nth, c.key);
}

std::vector<CommandKey> CommandKeys::changes() const
{
    std::vector<CommandKey> out;
    for (const CommandKey& k : keys_) {
        bool same = false;
        for (const CommandKey& d : defaults_)
            if (d.id == k.id && d.nth == k.nth)
                same = d.key == k.key;
        if (!same)
            out.push_back(k);
    }
    return out;
}

std::vector<CommandKey> CommandKeys::conflicts(const KeyCombo& key, int exceptId, int exceptNth) const
{
    std::vector<CommandKey> out;
    if (key.empty())
        return out;
    for (const CommandKey& k : keys_)
        if (k.key == key && !(k.id == exceptId && k.nth == exceptNth))
            out.push_back(k);
    return out;
}

} // namespace npp::core
