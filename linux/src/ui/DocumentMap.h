// Document Map — upstream DocumentMap: a zoomed-out view of the active
// document with the part shown in the editor marked (upstream's
// "view zone"). Clicking or dragging in the map scrolls the editor there.
// The map shares the editor's Scintilla document (SCI_SETDOCPOINTER) and
// draws the zone on an overlay, so nothing is added to the document.
#pragma once

#include "ui/EditorView.h"

#include <gtk/gtk.h>

#include <functional>

namespace npp::ui {

class DocumentMap {
public:
    // `restyle` applies the document's language and theme to the map view.
    explicit DocumentMap(std::function<void(EditorView& map, EditorView& source)> restyle);
    ~DocumentMap();

    GtkWidget* widget() const { return overlay_; }
    // Follows `source` (null: shows nothing). Call on tab switch and theme change.
    void attach(EditorView* source);
    // Call when the source scrolls, wraps or zooms.
    void sync();
    // Scrolls the source so that map line `mapY` (pixels) is centred, as a click does.
    void scrollTo(int mapY);
    EditorView& view() { return map_; }
    // The zone in map pixels (y, height), for tests.
    std::pair<int, int> zone() const { return {zoneY_, zoneH_}; }

private:
    std::function<void(EditorView&, EditorView&)> restyle_;
    EditorView map_;
    EditorView* source_ = nullptr;
    GtkWidget* overlay_ = nullptr;
    GtkWidget* zoneArea_ = nullptr;
    int zoneY_ = 0, zoneH_ = 0;
    bool dragging_ = false;
};

} // namespace npp::ui
