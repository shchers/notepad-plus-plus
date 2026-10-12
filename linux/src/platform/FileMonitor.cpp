#include "platform/FileMonitor.h"

#include <gio/gio.h>

namespace npp::platform {

FileMonitor::FileMonitor(const std::filesystem::path& file, std::function<void()> onChange) : onChange_(std::move(onChange))
{
    GFile* f = g_file_new_for_path(file.c_str());
    monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (!monitor_)
        return; // e.g. unsupported file system: no detection, as upstream on some network drives
    g_signal_connect(monitor_, "changed", G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer p) {
                         switch (ev) {
                         case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
                         case G_FILE_MONITOR_EVENT_DELETED:
                         case G_FILE_MONITOR_EVENT_CREATED:
                         case G_FILE_MONITOR_EVENT_RENAMED:
                         case G_FILE_MONITOR_EVENT_MOVED_IN:
                         case G_FILE_MONITOR_EVENT_MOVED_OUT:
                             static_cast<FileMonitor*>(p)->onChange_();
                             break;
                         default:
                             break;
                         }
                     }),
                     this);
}

FileMonitor::~FileMonitor()
{
    if (monitor_) {
        g_signal_handlers_disconnect_by_data(monitor_, this);
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
    }
}

} // namespace npp::platform
