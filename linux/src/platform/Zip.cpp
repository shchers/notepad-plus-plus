#include "platform/Zip.h"

#include "platform/FileIO.h"

#include <gio/gio.h>

#include <cstdint>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace npp::platform {

namespace {

std::uint32_t u16(const std::string& d, std::size_t at)
{
    return static_cast<std::uint8_t>(d[at]) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[at + 1])) << 8;
}

std::uint32_t u32(const std::string& d, std::size_t at)
{
    return u16(d, at) | u16(d, at + 2) << 16;
}

bool fail(std::string* error, const std::string& what)
{
    if (error)
        *error = what;
    return false;
}

// Raw deflate (zip method 8) through GZlibDecompressor.
bool inflate(const char* in, std::size_t inLen, std::string& out, std::string* error)
{
    GZlibDecompressor* z = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_RAW);
    std::size_t inPos = 0, outPos = 0;
    bool ok = false;
    for (;;) {
        gsize read = 0, written = 0;
        GError* err = nullptr;
        const GConverterResult r = g_converter_convert(G_CONVERTER(z), in + inPos, inLen - inPos, out.data() + outPos,
                                                       out.size() - outPos, G_CONVERTER_INPUT_AT_END, &read, &written, &err);
        inPos += read;
        outPos += written;
        if (r == G_CONVERTER_FINISHED) {
            ok = outPos == out.size();
            if (!ok)
                fail(error, "size mismatch");
            break;
        }
        if (r == G_CONVERTER_ERROR) {
            fail(error, err ? err->message : "invalid compressed data");
            if (err)
                g_error_free(err);
            break;
        }
        if (read == 0 && written == 0) {
            fail(error, "invalid compressed data");
            break;
        }
    }
    g_object_unref(z);
    return ok;
}

struct Entry {
    std::string name;
    std::uint32_t method, compSize, size, localOffset;
};

} // namespace

bool extractZip(const fs::path& zip, const fs::path& dest, std::string* error)
{
    std::string d;
    if (!readFile(zip, d, error))
        return false;
    // End of central directory: the last "PK\5\6", within the 64 KiB comment limit.
    if (d.size() < 22)
        return fail(error, "not a zip file");
    std::size_t eocd = std::string::npos;
    for (std::size_t i = d.size() - 22 + 1; i-- > 0 && d.size() - i <= 22 + 0xFFFF;)
        if (u32(d, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos)
        return fail(error, "not a zip file");
    const std::uint32_t count = u16(d, eocd + 10);
    std::size_t at = u32(d, eocd + 16);

    std::vector<Entry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > d.size() || u32(d, at) != 0x02014b50)
            return fail(error, "damaged zip file");
        Entry e{{}, u16(d, at + 10), u32(d, at + 20), u32(d, at + 24), u32(d, at + 42)};
        const std::uint32_t nameLen = u16(d, at + 28), extraLen = u16(d, at + 30), commentLen = u16(d, at + 32);
        if (at + 46 + nameLen > d.size())
            return fail(error, "damaged zip file");
        if (u16(d, at + 8) & 1)
            return fail(error, "encrypted zip files are not supported");
        if (e.compSize == 0xFFFFFFFF || e.size == 0xFFFFFFFF || e.localOffset == 0xFFFFFFFF)
            return fail(error, "ZIP64 files are not supported");
        e.name = d.substr(at + 46, nameLen);
        for (char& c : e.name)
            if (c == '\\')
                c = '/';
        // Everything must stay inside `dest`.
        const fs::path rel = fs::path(e.name).lexically_normal();
        if (e.name.empty() || rel.is_absolute() || rel.empty() || *rel.begin() == "..")
            return fail(error, "unsafe file name in the archive: " + e.name);
        if (e.method != 0 && e.method != 8)
            return fail(error, "unsupported compression in the archive: " + e.name);
        entries.push_back(std::move(e));
        at += 46 + nameLen + extraLen + commentLen;
    }

    std::error_code ec;
    fs::create_directories(dest, ec);
    for (const Entry& e : entries) {
        const fs::path target = dest / fs::path(e.name).lexically_normal();
        if (e.name.back() == '/') {
            fs::create_directories(target, ec);
            continue;
        }
        const std::size_t lh = e.localOffset;
        if (lh + 30 > d.size() || u32(d, lh) != 0x04034b50)
            return fail(error, "damaged zip file");
        const std::size_t data = lh + 30 + u16(d, lh + 26) + u16(d, lh + 28);
        if (data + e.compSize > d.size())
            return fail(error, "damaged zip file");
        std::string out(e.size, '\0');
        if (e.method == 0) {
            if (e.compSize != e.size)
                return fail(error, "damaged zip file");
            out.assign(d, data, e.size);
        } else if (e.size > 0 && !inflate(d.data() + data, e.compSize, out, error)) {
            if (error)
                *error = e.name + ": " + *error;
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream f(target, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f)
            return fail(error, "cannot write " + target.string());
    }
    return true;
}

} // namespace npp::platform
