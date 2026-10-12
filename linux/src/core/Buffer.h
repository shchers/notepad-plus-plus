// A document independent of the views showing it — the UI-free part of
// upstream ScintillaComponent/Buffer.h.
//
// The text lives in a Scintilla document. Upstream swaps one document in and
// out of each of its two views (SCI_SETDOCPOINTER); the port gives every tab
// its own Scintilla widget and lets clones share the document the same way,
// so one Buffer can be shown in both views. Modified state comes from the
// document's save point, plus `forcedDirty` for changes that are not edits.
#pragma once

#include "core/LanguageCatalog.h"
#include "platform/FileIO.h"
#include "platform/TextCodec.h"

#include <filesystem>
#include <string>

namespace npp::core {

struct Buffer {
    std::filesystem::path path;    // empty for "new N"
    std::string untitledName;
    platform::TextEncoding encoding;
    const Language* language = nullptr;
    bool userReadOnly = false;     // Edit > Read-Only and -ro

    // Dirty without an undoable edit: encoding conversion, or text restored
    // from a snapshot. Cleared on save and reload.
    bool forcedDirty = false;

    platform::FileStamp diskStamp; // file state at the last load/save
    bool externalChangePending = false;

    // Session snapshot (upstream backup/<name>@<timestamp>): where unsaved
    // text is kept, and whether it lags behind the editor.
    std::filesystem::path snapshotPath;
    bool snapshotStale = false;

    bool isUntitled() const { return path.empty(); }
    std::string name() const { return isUntitled() ? untitledName : path.filename().string(); }
    // Untitled name or full path: what the title bar and session.xml show.
    std::string fullName() const { return isUntitled() ? untitledName : path.string(); }
};

} // namespace npp::core
