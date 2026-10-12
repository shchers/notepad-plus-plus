// Clipboard History — upstream ClipboardHistoryPanel: the texts copied to
// the clipboard (by any application) while the editor runs, newest first,
// without duplicates. Double-click pastes an entry into the document.
#pragma once

#include <gtk/gtk.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace npp::ui {

class ClipboardHistoryPanel {
public:
    explicit ClipboardHistoryPanel(std::function<void(const std::string& text)> paste);
    ~ClipboardHistoryPanel();
    GtkWidget* widget() const { return box_; }
    void add(const std::string& text);
    const std::deque<std::string>& entries() const { return entries_; }
    static constexpr std::size_t kMax = 30;

private:
    void rebuild();
    std::function<void(const std::string&)> paste_;
    std::deque<std::string> entries_;
    GtkWidget* box_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkListStore* store_ = nullptr;
    GtkClipboard* clipboard_ = nullptr;
    gulong handler_ = 0;
    // Clipboard reads finish asynchronously, maybe after the panel is gone.
    std::shared_ptr<ClipboardHistoryPanel*> alive_;
};

} // namespace npp::ui
