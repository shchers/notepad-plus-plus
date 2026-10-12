#include "platform/FileIO.h"

#include <gio/gio.h>

#include <ctime>
#include <memory>

namespace fs = std::filesystem;

namespace npp::platform {

namespace {

constexpr std::uint64_t kFileTimeUnixEpoch = 116444736000000000ULL; // 1970-01-01 in FILETIME ticks

bool fail(std::string* error, GError* err, const char* fallback)
{
    if (error)
        *error = err ? err->message : fallback;
    if (err)
        g_error_free(err);
    return false;
}

} // namespace

FileStamp statFile(const fs::path& p)
{
    FileStamp st;
    std::unique_ptr<GFile, decltype(&g_object_unref)> f(g_file_new_for_path(p.c_str()), &g_object_unref);
    GFileInfo* info = g_file_query_info(f.get(),
                                        G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC
                                        "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (!info)
        return st;
    st.exists = true;
    st.mtimeUs = static_cast<std::int64_t>(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) * 1000000 +
                 g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
    st.size = g_file_info_get_size(info);
    g_object_unref(info);
    return st;
}

bool readFile(const fs::path& p, std::string& out, std::string* error)
{
    gchar* contents = nullptr;
    gsize len = 0;
    GError* err = nullptr;
    if (!g_file_get_contents(p.c_str(), &contents, &len, &err))
        return fail(error, err, "read failed");
    out.assign(contents, len);
    g_free(contents);
    return true;
}

bool writeFileAtomic(const fs::path& p, std::string_view bytes, std::string* error)
{
    std::unique_ptr<GFile, decltype(&g_object_unref)> f(g_file_new_for_path(p.c_str()), &g_object_unref);
    GError* err = nullptr;
    if (!g_file_replace_contents(f.get(), bytes.data(), bytes.size(), nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, nullptr, &err))
        return fail(error, err, "write failed");
    return true;
}

bool copyFile(const fs::path& from, const fs::path& to, std::string* error)
{
    std::unique_ptr<GFile, decltype(&g_object_unref)> src(g_file_new_for_path(from.c_str()), &g_object_unref);
    std::unique_ptr<GFile, decltype(&g_object_unref)> dst(g_file_new_for_path(to.c_str()), &g_object_unref);
    GError* err = nullptr;
    if (!g_file_copy(src.get(), dst.get(), GFileCopyFlags(G_FILE_COPY_OVERWRITE | G_FILE_COPY_TARGET_DEFAULT_PERMS), nullptr,
                     nullptr, nullptr, &err))
        return fail(error, err, "copy failed");
    return true;
}

std::uint64_t toFileTime(std::int64_t mtimeUs)
{
    return static_cast<std::uint64_t>(mtimeUs) * 10 + kFileTimeUnixEpoch;
}

std::int64_t fromFileTime(std::uint64_t fileTime)
{
    if (fileTime < kFileTimeUnixEpoch)
        return 0;
    return static_cast<std::int64_t>((fileTime - kFileTimeUnixEpoch) / 10);
}

std::string backupTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d_%H%M%S", &tm);
    return buf;
}

} // namespace npp::platform
