#include "core/Workspace.h"

#include <pugixml.hpp>

#include <algorithm>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

fs::path absoluteFor(std::string name, const fs::path& wsDir)
{
    // A workspace written on Windows uses backslashes; drive paths stay as written.
    if (name.size() < 2 || name[1] != ':')
        std::replace(name.begin(), name.end(), '\\', '/');
    fs::path p = name;
    return p.is_absolute() ? p.lexically_normal() : (wsDir / p).lexically_normal();
}

void readChildren(const pugi::xml_node& parent, std::vector<WorkspaceNode>& out, const fs::path& wsDir)
{
    for (const pugi::xml_node n : parent.children()) {
        const std::string tag = n.name();
        if (tag == "Folder") {
            WorkspaceNode f{WorkspaceNode::Kind::Folder, n.attribute("name").as_string(), {}, {}};
            readChildren(n, f.children, wsDir);
            out.push_back(std::move(f));
        } else if (tag == "File") {
            const std::string name = n.attribute("name").as_string();
            if (name.empty())
                continue;
            const fs::path p = absoluteFor(name, wsDir);
            out.push_back({WorkspaceNode::Kind::File, p.filename().string(), p, {}});
        }
    }
}

// Upstream getRelativePath: below the workspace's folder, the rest of the path.
std::string relativeName(const fs::path& file, const fs::path& wsDir)
{
    const std::string f = file.lexically_normal().string(), d = wsDir.lexically_normal().string();
    if (!d.empty() && f.size() > d.size() && f.compare(0, d.size(), d) == 0 && (d.back() == '/' || f[d.size()] == '/'))
        return f.substr(d.back() == '/' ? d.size() : d.size() + 1);
    return f;
}

void writeChildren(pugi::xml_node parent, const std::vector<WorkspaceNode>& nodes, const fs::path& wsDir)
{
    for (const WorkspaceNode& n : nodes) {
        if (n.kind == WorkspaceNode::Kind::File) {
            parent.append_child("File").append_attribute("name") = relativeName(n.path, wsDir).c_str();
        } else {
            pugi::xml_node f = parent.append_child("Folder");
            f.append_attribute("name") = n.name.c_str();
            writeChildren(f, n.children, wsDir);
        }
    }
}

void collect(const std::vector<WorkspaceNode>& nodes, std::vector<fs::path>& out)
{
    for (const WorkspaceNode& n : nodes) {
        if (n.kind == WorkspaceNode::Kind::File)
            out.push_back(n.path);
        else
            collect(n.children, out);
    }
}

} // namespace

bool loadWorkspace(const fs::path& file, Workspace& out, std::string* error)
{
    pugi::xml_document doc;
    if (const pugi::xml_parse_result res = doc.load_file(file.c_str()); !res) {
        if (error)
            *error = file.string() + ": " + res.description();
        return false;
    }
    const pugi::xml_node root = doc.child("NotepadPlus");
    if (!root) {
        if (error)
            *error = file.string() + ": not a Notepad++ workspace";
        return false;
    }
    out = {};
    const fs::path dir = fs::absolute(file).parent_path();
    for (const pugi::xml_node p : root.children("Project")) {
        WorkspaceNode proj{WorkspaceNode::Kind::Project, p.attribute("name").as_string(), {}, {}};
        readChildren(p, proj.children, dir);
        out.projects.push_back(std::move(proj));
    }
    return true;
}

bool saveWorkspace(const fs::path& file, const Workspace& ws, std::string* error)
{
    pugi::xml_document doc;
    pugi::xml_node root = doc.append_child("NotepadPlus");
    const fs::path dir = fs::absolute(file).parent_path();
    for (const WorkspaceNode& p : ws.projects) {
        pugi::xml_node n = root.append_child("Project");
        n.append_attribute("name") = p.name.c_str();
        writeChildren(n, p.children, dir);
    }
    // Upstream NppXml::saveFileProject: indented, no declaration.
    if (!doc.save_file(file.c_str(), "    ", pugi::format_indent | pugi::format_no_declaration | pugi::format_save_file_text)) {
        if (error)
            *error = "An error occurred while writing your workspace file.\nYour workspace has not been saved.";
        return false;
    }
    return true;
}

std::vector<fs::path> workspaceFiles(const Workspace& ws)
{
    std::vector<fs::path> out;
    for (const WorkspaceNode& p : ws.projects)
        collect(p.children, out);
    return out;
}

} // namespace npp::core
