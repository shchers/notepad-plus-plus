// Document List — upstream VerticalFileSwitcher: the open documents in tab
// order, both views, with name and extension (the path as tooltip). A click switches to the
// document, a middle click closes it; the active one is selected.
#pragma once

#include "core/Translator.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace npp::ui {

class DocListPanel {
public:
    struct Entry {
        int view = 0;
        int index = 0;        // tab index in that view
        std::string name;
        std::string path;     // full name (path, or "new 1")
        bool modified = false;
        bool active = false;
    };
    DocListPanel(const core::Translator& tr, std::function<void(int view, int index)> activate,
                 std::function<void(int view, int index)> close);
    ~DocListPanel();
    GtkWidget* widget() const { return box_; }
    void update(const std::vector<Entry>& entries);
    std::string dump() const;

private:
    std::function<void(int, int)> activate_, close_;
    GtkWidget* box_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkListStore* store_ = nullptr;
    bool updating_ = false;
};

} // namespace npp::ui
