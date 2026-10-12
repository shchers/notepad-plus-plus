// File system primitives for documents — replaces the Win32 CreateFile /
// GetFileAttributesEx / CopyFile calls in upstream FileManager and NppIO.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace npp::platform {

// What upstream remembers about a file on disk to notice outside changes.
struct FileStamp {
    bool exists = false;
    std::int64_t mtimeUs = 0; // modification time, microseconds since the Unix epoch
    std::int64_t size = -1;   // -1 = unknown (e.g. restored from session.xml)

    // Same file state. An unknown size on either side is not compared.
    bool sameAs(const FileStamp& o) const
    {
        return exists == o.exists && (!exists || (mtimeUs == o.mtimeUs && (size < 0 || o.size < 0 || size == o.size)));
    }
};

FileStamp statFile(const std::filesystem::path& p);

bool readFile(const std::filesystem::path& p, std::string& out, std::string* error);
// Atomic replace through GIO (temporary file + rename); keeps the existing
// file's permissions.
bool writeFileAtomic(const std::filesystem::path& p, std::string_view bytes, std::string* error);
bool copyFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error);

// session.xml stores Windows FILETIME (100 ns ticks since 1601-01-01) split
// into two 32-bit halves, like upstream.
std::uint64_t toFileTime(std::int64_t mtimeUs);
std::int64_t fromFileTime(std::uint64_t fileTime);

// "%Y-%m-%d_%H%M%S" in local time, as upstream names backup files.
std::string backupTimestamp();

} // namespace npp::platform
