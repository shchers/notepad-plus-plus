#include "core/FileManager.h"

#include "platform/Paths.h"

#include <system_error>

namespace fs = std::filesystem;

namespace npp::core {

std::optional<LoadedFile> loadFile(const fs::path& p, int codepage, std::string* error)
{
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        if (error)
            *error = p.string() + " is a directory.";
        return std::nullopt;
    }
    LoadedFile out;
    out.stamp = platform::statFile(p);
    std::string raw;
    if (out.stamp.exists && !platform::readFile(p, raw, error))
        return std::nullopt;
    // Windows' Unicode code pages (Encoding > UTF-8 / UTF-16 on a legacy
    // document) besides the legacy ones session.xml stores.
    const std::string cs = codepage == 65001 ? "UTF-8"
                         : codepage == 1200  ? "UTF-16LE"
                         : codepage == 1201  ? "UTF-16BE"
                                             : platform::charsetForCodepage(codepage);
    if (!cs.empty()) {
        if (auto d = platform::decodeAs(raw, cs, nullptr)) {
            out.text = std::move(*d);
            return out;
        }
        // Bytes do not fit the remembered code page any more: detect instead.
    }
    out.text = platform::decode(raw);
    return out;
}

std::optional<fs::path> backupBeforeSave(const fs::path& file, platform::SaveBackup mode, const fs::path& customDir,
                                         std::string* error)
{
    using platform::SaveBackup;
    std::error_code ec;
    if (mode == SaveBackup::None || !fs::is_regular_file(file, ec))
        return fs::path{};
    fs::path dir = customDir.empty() ? file.parent_path() : customDir;
    if (mode == SaveBackup::Verbose && customDir.empty())
        dir /= "nppBackup";
    fs::create_directories(dir, ec);
    const std::string name = file.filename().string();
    const fs::path target = dir / (mode == SaveBackup::Simple ? name + ".bak"
                                                              : name + "." + platform::backupTimestamp() + ".bak");
    if (!platform::copyFile(file, target, error))
        return std::nullopt;
    return target;
}

std::optional<platform::FileStamp> saveFile(const fs::path& target, std::string_view utf8, const platform::TextEncoding& enc,
                                            std::string* error, bool* unrepresentable)
{
    auto bytes = platform::encode(utf8, enc, error);
    if (unrepresentable)
        *unrepresentable = !bytes;
    if (!bytes || !platform::writeFileAtomic(target, *bytes, error))
        return std::nullopt;
    return platform::statFile(target);
}

fs::path snapshotDir()
{
    fs::path dir = platform::userConfigDir() / "backup";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

bool isSnapshotPath(const fs::path& p)
{
    if (p.empty() || !p.is_absolute())
        return false;
    const fs::path dir = snapshotDir().lexically_normal();
    const fs::path norm = p.lexically_normal();
    return norm.parent_path() == dir;
}

bool writeSnapshot(Buffer& buf, std::string_view utf8, std::string* error)
{
    if (!isSnapshotPath(buf.snapshotPath)) {
        // Upstream naming: "<file name>@<timestamp>"; disambiguate within a second.
        const std::string base = buf.name() + "@" + platform::backupTimestamp();
        fs::path p = snapshotDir() / base;
        std::error_code ec;
        for (int i = 2; fs::exists(p, ec); ++i)
            p = snapshotDir() / (base + "-" + std::to_string(i));
        buf.snapshotPath = p;
    }
    auto bytes = platform::encode(utf8, buf.encoding, nullptr);
    if (!bytes)
        bytes = platform::encode(utf8, {"UTF-8", platform::Bom::Utf8}, nullptr);
    if (!bytes || !platform::writeFileAtomic(buf.snapshotPath, *bytes, error))
        return false;
    buf.snapshotStale = false;
    return true;
}

void deleteSnapshot(Buffer& buf)
{
    if (isSnapshotPath(buf.snapshotPath)) {
        std::error_code ec;
        fs::remove(buf.snapshotPath, ec);
    }
    buf.snapshotPath.clear();
    buf.snapshotStale = false;
}

} // namespace npp::core
