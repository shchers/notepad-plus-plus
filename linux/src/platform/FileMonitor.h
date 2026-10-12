// Watches one file for outside changes — replaces upstream's
// ReadDirectoryChanges thread (WinControls/ReadDirectoryChanges) with
// GFileMonitor (inotify on Linux). Events arrive on the GLib main loop.
#pragma once

#include <filesystem>
#include <functional>

typedef struct _GFileMonitor GFileMonitor;

namespace npp::platform {

class FileMonitor {
public:
    // `onChange` runs after the file was written, replaced, created or deleted.
    // It may fire for our own saves too; compare FileStamps to filter them.
    FileMonitor(const std::filesystem::path& file, std::function<void()> onChange);
    ~FileMonitor();
    FileMonitor(const FileMonitor&) = delete;
    FileMonitor& operator=(const FileMonitor&) = delete;

private:
    GFileMonitor* monitor_ = nullptr;
    std::function<void()> onChange_;
};

} // namespace npp::platform
