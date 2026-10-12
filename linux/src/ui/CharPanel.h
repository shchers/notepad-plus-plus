// Character Panel — upstream AnsiCharPanel ("ASCII Codes Insertion Panel"):
// the 256 codes of the document's single-byte character set (Windows-1252
// for Unicode documents, standing in for Windows' ANSI code page), with
// their HTML names and numbers. Double-click inserts the character, or the
// HTML form from the HTML columns, as upstream.
#pragma once

#include "core/Translator.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace npp::ui {

class CharPanel {
public:
    CharPanel(const core::Translator& tr, std::function<void(const std::string& utf8)> insert);
    ~CharPanel();
    GtkWidget* widget() const { return box_; }
    // Re-labels 128-255 for a charset ("" or a Unicode one: Windows-1252).
    void setCharset(const std::string& charset);
    const std::string& charset() const { return charset_; }
    // What a double-click on row `code`, column `col` (0 value .. 5 HTML hex) inserts.
    std::string textFor(int code, int col) const;

private:
    std::function<void(const std::string&)> insert_;
    std::string charset_;
    GtkWidget* box_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkListStore* store_ = nullptr;
};

} // namespace npp::ui
