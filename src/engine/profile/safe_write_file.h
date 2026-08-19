#pragma once

#include <filesystem>
#include <string>

namespace engine::profile {

// Atomically replace `target` with `content` (MO2's SafeWriteFile pattern,
// Qt-free).
//
// The content is written to a uniquely-named temp file in the SAME directory
// as the target, flushed, then renamed over the target. Rename within one
// directory is atomic on POSIX; on Windows MoveFileExW with
// MOVEFILE_REPLACE_EXISTING provides the same replace semantics. A reader
// therefore never observes a partially-written profile file.
//
// The parent directory is created if missing. Returns true on success; on
// failure the target is left untouched and the temp file is removed.
[[nodiscard]] bool safe_write_file(const std::filesystem::path& target,
                                   const std::string& content);

}  // namespace engine::profile