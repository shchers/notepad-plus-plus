// Search > Incremental Search — upstream FindIncrementDlg: a bar under the
// editors that searches as you type, with Match case, Highlight all and a
// match count. The search itself is core::SearchEngine, as in the Find dialog.
#pragma once

#include "core/Translator.h"
#include "ui/EditorView.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace npp::ui {

class FindBar {
public:
    // `activeView` returns the editor the bar works on (may be null).
    FindBar(std::function<EditorView*()> activeView, const core::Translator& tr);

    GtkWidget* widget() const { return bar_; }
    void retranslate();

    void show();   // with the selection as the text, focused
    void hide();   // clears the highlight and goes back to the editor
    bool isVisible() const;

    // As typing does (from the start of the selection), or Next/Previous.
    void search(bool fromSelectionStart, bool forward);
    void setText(const std::string& s);
    void setMatchCase(bool on);
    void setHighlightAll(bool on);
    const std::string& status() const { return status_; }

private:
    void highlight();
    void setStatus(const std::string& s, bool error);

    std::function<EditorView*()> activeView_;
    const core::Translator& tr_;
    std::string status_;
    GtkWidget* bar_ = nullptr;
    GtkWidget* label_ = nullptr;
    GtkWidget* entry_ = nullptr;
    GtkWidget* matchCase_ = nullptr;
    GtkWidget* highlightAll_ = nullptr;
    GtkWidget* statusLabel_ = nullptr;
};

} // namespace npp::ui
