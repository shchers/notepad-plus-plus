// Loading and saving documents, backups — the Win32-free logic of upstream
// FileManager (Buffer.cpp) and Notepad_plus::doSave / fileSave (NppIO.cpp).
#pragma once

#include "core/Buffer.h"
#include "core/UndoState.h"
#include "platform/Settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace npp::core {

struct LoadedFile {
    platform::DecodedText text;
    platform::FileStamp stamp;
};

// Reads and decodes a file. `codepage` forces a Windows code page (as stored
// in session.xml, or 65001 / 1200 / 1201 for UTF-8 / UTF-16 LE / BE); -1
// detects. A missing file loads as empty text with a
// stamp whose `exists` is false.
std::optional<LoadedFile> loadFile(const std::filesystem::path& p, int codepage, std::string* error);

// Upstream "Backup on save": copies the current file before it is replaced.
//   Simple:  <dir>/<name>.bak
//   Verbose: <dir>/nppBackup/<name>.<YYYY-MM-DD_HHMMSS>.bak
// where <dir> is `customDir` or the file's directory. Returns the backup path,
// empty when there was nothing to back up; std::nullopt on failure.
std::optional<std::filesystem::path> backupBeforeSave(const std::filesystem::path& file, platform::SaveBackup mode,
                                                      const std::filesystem::path& customDir, std::string* error);

// Encodes and atomically writes `utf8`. Returns the new disk stamp.
// `*unrepresentable` is set when the text does not fit the encoding.
std::optional<platform::FileStamp> saveFile(const std::filesystem::path& target, std::string_view utf8,
                                            const platform::TextEncoding& enc, std::string* error,
                                            bool* unrepresentable = nullptr);

// ---- Session snapshots (upstream "periodic backup") ------------------------

// <userConfigDir>/backup, created on demand.
std::filesystem::path snapshotDir();
// Only snapshot files inside snapshotDir() are read or deleted, as upstream
// confines session.xml's backupFilePath to its backup folder.
bool isSnapshotPath(const std::filesystem::path& p);
// Writes the buffer's unsaved text to its snapshot file, naming it
// "<name>@<timestamp>" on first use. Encoded like the buffer, falling back to
// UTF-8 with BOM when the text does not fit the buffer's encoding.
// `undo`, the editor's undo history, is kept beside it ("<snapshot>.undo");
// without one any older history file is removed.
bool writeSnapshot(Buffer& buf, std::string_view utf8, const UndoState* undo, std::string* error);
// The snapshot and its undo history.
void deleteSnapshot(Buffer& buf);
// Removes undo history files whose snapshot is gone (left by a crash).
void removeOrphanUndoStates();

} // namespace npp::core
