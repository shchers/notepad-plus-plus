// Reading .zip archives — what Plugins Admin needs to install a plugin
// shipped as a zip, as upstream's plugins are. Stored and deflated entries
// (the methods every zip tool writes by default); no ZIP64, no encryption.
// Deflate goes through GIO's zlib converter, so there is no new dependency.
#pragma once

#include <filesystem>
#include <string>

namespace npp::platform {

// Extracts `zip` into `dest` (created if needed). Windows separators in
// names are accepted; names that would leave `dest` (absolute, "..") make it
// fail before anything is written.
bool extractZip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::string* error);

} // namespace npp::platform
