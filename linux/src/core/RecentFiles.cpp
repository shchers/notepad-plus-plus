#include "core/RecentFiles.h"

#include <algorithm>

namespace npp::core {

void RecentFiles::add(const std::string& file)
{
    if (file.empty() || max_ == 0)
        return;
    remove(file);
    items_.insert(items_.begin(), file);
    if (items_.size() > max_)
        items_.resize(max_);
}

void RecentFiles::remove(const std::string& file)
{
    items_.erase(std::remove(items_.begin(), items_.end(), file), items_.end());
}

void RecentFiles::setMax(std::size_t max)
{
    max_ = max;
    if (items_.size() > max_)
        items_.resize(max_);
}

void RecentFiles::assign(const std::vector<std::string>& items)
{
    items_.clear();
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        add(*it);
}

} // namespace npp::core
