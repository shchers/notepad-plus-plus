// Function List panel — upstream FunctionListPanel: the functions of the
// active document, grouped under their class, with a filter, Sort and
// Reload. Double-click (or Enter) brings the function into view. The list
// follows the active document and refreshes shortly after edits.
#pragma once

#include "core/FunctionParser.h"
#include "core/Translator.h"
#include "ui/EditorView.h"

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace npp::ui {

class FunctionListPanel {
public:
    struct Document {
        EditorView* view = nullptr;
        std::string name;     // root label: file name
        std::string langName; // langs.model.xml name
        std::string udlName;  // UDL name, if any
    };

    FunctionListPanel(core::FunctionParsers& parsers, const core::Translator& tr, std::function<Document()> current);
    ~FunctionListPanel();

    GtkWidget* widget() const { return box_; }
    void reload();          // parse the current document again
    void scheduleReload();  // after edits: once typing pauses
    void setSorted(bool on);
    bool sorted() const { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sort_)); }
    void setFilter(const std::string& text);
    const std::vector<core::FunctionInfo>& functions() const { return infos_; }
    // Visible rows as "name" / "  child" lines, for tests.
    std::string dump() const;
    bool openRow(GtkTreePath* path);

private:
    void rebuild();
    core::FunctionParsers& parsers_;
    const core::Translator& tr_;
    std::function<Document()> current_;
    std::vector<core::FunctionInfo> infos_;
    std::string root_;
    GtkWidget* box_ = nullptr;
    GtkWidget* search_ = nullptr;
    GtkWidget* sort_ = nullptr;
    GtkWidget* tree_ = nullptr;
    GtkTreeStore* store_ = nullptr;
    guint timer_ = 0;
};

} // namespace npp::ui
