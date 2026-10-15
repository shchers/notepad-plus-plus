// Undo history kept with a session snapshot, the port's addition: upstream
// restores unsaved text after a restart but not its undo history. Scintilla's
// undo save and restore API (SCI_GETUNDOACTIONS, SCI_PUSHUNDOACTIONTYPE, ...)
// reads and rebuilds the history; this is its file, "<snapshot>.undo" next to
// the snapshot.
//
// The history only applies to the exact text it was taken from, so the file
// records that text's length and hash and is ignored when they differ.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npp::core {

struct UndoAction {
    int type = 0;               // SCI_GETUNDOACTIONTYPE: insert/remove plus the coalesce flag
    std::int64_t position = 0;
    std::string text;
};

struct UndoState {
    std::vector<UndoAction> actions;
    int savePoint = -1, detach = -1, tentative = -1, current = 0;
    std::uint64_t textLength = 0, textHash = 0; // the document the history leads to

    bool empty() const { return actions.empty(); }
    bool fits(std::string_view text) const;
};

std::uint64_t undoTextHash(std::string_view text);

// Histories with more text than this are not kept: rewriting them on every
// snapshot tick would cost more than they are worth.
constexpr std::size_t kMaxUndoStateBytes = 64u << 20;

std::filesystem::path undoStatePath(const std::filesystem::path& snapshot);
bool writeUndoState(const std::filesystem::path& file, const UndoState& s, std::string* error);
std::optional<UndoState> readUndoState(const std::filesystem::path& file);

} // namespace npp::core
