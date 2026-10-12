// Project Panel workspaces — upstream ProjectPanel's .workspace XML
// (<NotepadPlus><Project name><Folder name><File name/>), read and written in
// the same format. File names are stored relative to the workspace file when
// they lie below its folder, as upstream does; Windows backslashes in a
// workspace made on Windows are accepted.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace npp::core {

struct WorkspaceNode {
    enum class Kind { Project, Folder, File };
    Kind kind = Kind::File;
    std::string name;            // Project / Folder label; File: display name
    std::filesystem::path path;  // File: absolute path
    std::vector<WorkspaceNode> children;
    bool operator==(const WorkspaceNode&) const = default;
};

struct Workspace {
    std::vector<WorkspaceNode> projects;
    bool operator==(const Workspace&) const = default;
};

bool loadWorkspace(const std::filesystem::path& file, Workspace& out, std::string* error = nullptr);
bool saveWorkspace(const std::filesystem::path& file, const Workspace& ws, std::string* error = nullptr);
// Every file of the workspace (Find in Projects), in tree order.
std::vector<std::filesystem::path> workspaceFiles(const Workspace& ws);

} // namespace npp::core
