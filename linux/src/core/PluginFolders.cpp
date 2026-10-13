#include "core/PluginFolders.h"

#include "platform/FileIO.h"
#include "platform/Zip.h"

#include <unistd.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace npp::core {

namespace {

constexpr const char* kDisabled = "disabled"; // upstream's folder name

bool fail(std::string* error, const std::string& what)
{
    if (error)
        *error = what;
    return false;
}

bool isLibrary(const fs::path& p)
{
    std::error_code ec;
    return p.extension() == ".so" && fs::is_regular_file(p, ec);
}

void scan(const fs::path& base, const fs::path& root, bool disabled, bool writable, std::vector<PluginFolder>& out)
{
    std::error_code ec;
    std::vector<PluginFolder> found;
    for (const fs::directory_entry& e : fs::directory_iterator(base, ec)) {
        const std::string name = e.path().filename().string();
        if (!disabled && isLibrary(e.path())) { // copied without its folder
            found.push_back({e.path().stem().string(), root, base, e.path(), false, writable, true, pluginVersion(e.path())});
            continue;
        }
        if (!e.is_directory(ec) || (!disabled && name == kDisabled))
            continue;
        const fs::path file = e.path() / (name + ".so");
        if (isLibrary(file))
            found.push_back({name, root, e.path(), file, disabled, writable, false, pluginVersion(file)});
    }
    std::sort(found.begin(), found.end(), [](const PluginFolder& a, const PluginFolder& b) { return a.name < b.name; });
    out.insert(out.end(), found.begin(), found.end());
}

// Moves `from` to `to`, replacing it.
bool replaceWith(const fs::path& from, const fs::path& to, std::string* error)
{
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) { // another file system: copy
        ec.clear();
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
            return fail(error, "cannot install into " + to.string() + ": " + ec.message());
        fs::remove_all(from, ec);
    }
    return true;
}

} // namespace

std::string pluginVersion(const fs::path& so)
{
    std::string d;
    if (!platform::readFile(so, d, nullptr))
        return {};
    static const std::string tag = "NppGtkPluginVersion:";
    const std::size_t at = d.find(tag);
    if (at == std::string::npos)
        return {};
    std::string v;
    for (std::size_t i = at + tag.size(); i < d.size() && d[i] != '\0' && v.size() < 64; ++i) {
        if (static_cast<unsigned char>(d[i]) < 0x20)
            return {};
        v += d[i];
    }
    return v;
}

bool canModify(const fs::path& root)
{
    fs::path p = root;
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec)) {
        if (p == p.parent_path())
            break;
        p = p.parent_path();
    }
    return !p.empty() && access(p.c_str(), W_OK) == 0;
}

std::vector<PluginFolder> listPlugins(const std::vector<fs::path>& roots)
{
    std::vector<PluginFolder> out;
    for (const fs::path& root : roots) {
        const bool writable = canModify(root);
        scan(root, root, false, writable, out);
        scan(root / kDisabled, root, true, writable, out);
    }
    return out;
}

bool deactivatePlugin(const PluginFolder& p, std::string* error)
{
    if (p.misplaced)
        return fail(error, p.file.filename().string() + " is not installed in a folder of its own");
    if (p.disabled)
        return true;
    return replaceWith(p.dir, p.root / kDisabled / p.name, error);
}

bool activatePlugin(const PluginFolder& p, std::string* error)
{
    if (!p.disabled)
        return true;
    return replaceWith(p.dir, p.root / p.name, error);
}

bool removePlugin(const PluginFolder& p, std::string* error)
{
    std::error_code ec;
    const fs::path what = p.misplaced ? p.file : p.dir; // never the plugins directory itself
    fs::remove_all(what, ec);
    if (ec)
        return fail(error, "cannot remove " + what.string() + ": " + ec.message());
    return true;
}

std::optional<std::string> installPlugin(const fs::path& source, const fs::path& root, std::string* error)
{
    std::error_code ec;
    const fs::path staging = root / ".installing";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        fail(error, "cannot create " + staging.string() + ": " + ec.message());
        return std::nullopt;
    }
    auto abandon = [&](const std::string& what) -> std::optional<std::string> {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        fail(error, what);
        return std::nullopt;
    };

    std::string name;
    fs::path content; // the plugin's folder inside the staging area
    if (fs::is_directory(source, ec)) {
        name = source.filename().string();
        if (name.empty())
            name = source.parent_path().filename().string();
        if (!isLibrary(source / (name + ".so")))
            return abandon(source.string() + " does not contain " + name + ".so");
        content = staging / name;
        fs::copy(source, content, fs::copy_options::recursive, ec);
    } else if (source.extension() == ".so") {
        name = source.stem().string();
        content = staging / name;
        fs::create_directories(content, ec);
        fs::copy_file(source, content / (name + ".so"), ec);
    } else if (source.extension() == ".zip") {
        const fs::path unpacked = staging / "zip";
        std::string err;
        if (!platform::extractZip(source, unpacked, &err))
            return abandon(source.filename().string() + ": " + err);
        // <Name>.so at the top (upstream's layout), else one <Name>/<Name>.so folder.
        std::vector<fs::path> top, dirs;
        for (const fs::directory_entry& e : fs::directory_iterator(unpacked, ec))
            (e.is_directory(ec) ? dirs : top).push_back(e.path());
        std::vector<fs::path> libs;
        std::copy_if(top.begin(), top.end(), std::back_inserter(libs), [](const fs::path& p) { return p.extension() == ".so"; });
        if (libs.size() == 1) {
            name = libs.front().stem().string();
            content = unpacked;
        } else if (libs.empty() && dirs.size() == 1 && isLibrary(dirs.front() / (dirs.front().filename().string() + ".so"))) {
            name = dirs.front().filename().string();
            content = dirs.front();
        } else {
            return abandon(source.filename().string() + " does not hold one plugin (<Name>.so, or a <Name>/ folder with <Name>.so)");
        }
    } else {
        return abandon(source.string() + " is not a plugin (.so), a plugin folder or a .zip");
    }
    if (ec)
        return abandon("cannot copy " + source.string() + ": " + ec.message());
    if (name.empty() || name == kDisabled || name.front() == '.')
        return abandon("\"" + name + "\" cannot be a plugin name");

    std::string err;
    fs::remove_all(root / kDisabled / name, ec); // the new one replaces a deactivated one too
    if (!replaceWith(content, root / name, &err))
        return abandon(err);
    fs::remove_all(staging, ec);
    return name;
}

} // namespace npp::core
