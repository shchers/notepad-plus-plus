// Most-recently-used file list — the model behind upstream lastRecentFileList
// (File menu history, "Restore Recent Closed File"). Upstream semantics: a
// file enters the list when it is closed and leaves it when it is opened.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace npp::core {

class RecentFiles {
public:
    explicit RecentFiles(std::size_t max = 10) : max_(max) {}

    void add(const std::string& file);      // to the front; moves an existing entry
    void remove(const std::string& file);
    void clear() { items_.clear(); }
    void setMax(std::size_t max);
    void assign(const std::vector<std::string>& items); // most recent first

    const std::vector<std::string>& items() const { return items_; }
    bool empty() const { return items_.empty(); }
    std::size_t max() const { return max_; }

private:
    std::size_t max_;
    std::vector<std::string> items_;
};

} // namespace npp::core
