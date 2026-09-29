#pragma once

// 7-Zip IArchive extraction backend for the engine archive path.
//
// Qt-free by construction: the 7-Zip COM-style interfaces are confined to
// sevenzip_backend.cpp, and nothing in this header names a 7-Zip type. The
// engine links lib7zip (LGPL-2.1-or-later, with the unRAR restriction) and
// only ever *decompresses* RAR, which the restriction permits.
//
// What is deliberately not here: the create-and-save path, per-entry multiple
// outputs, symlink/hardlink entries, and 7-Zip's two-kind progress model. The
// extractor's single done/total byte counter is the contract on both sides,
// and an entry is written as a plain file, matching the libarchive path.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "engine/mod/archive/archive_extractor.h"

namespace engine {

// The extraction engine that will read a given archive.
//
// The decision is made from the archive's leading bytes, never its filename: a
// mod named "Texture Pack.7z" that is really a zip is still a zip. RAR (either
// generation) and 7z go to 7-Zip; everything else stays on libarchive, which is
// the incumbent and already has the test coverage. See route_archive().
enum class ArchiveEngine {
  kLibarchive,  // libarchive: the default reader
  kSevenZip,    // 7-Zip (lib7zip): RAR and 7z
};

// The engine chosen for `archive`. Pure and content-based, so it is assertable
// in a test without extracting anything.
[[nodiscard]] ArchiveEngine route_archive(const std::filesystem::path &archive);

// Human-readable engine name, for the log line and for error messages so a
// failure names the reader that actually ran.
[[nodiscard]] const char *archive_engine_name(ArchiveEngine engine);

// The 7-Zip handler name for `archive`, or an empty string when 7-Zip does not
// handle the format. "Rar5" and "Rar" are 7-Zip's own names for the two RAR
// generations; found by name through GetNumberOfFormats/GetHandlerProperty2 so
// no handler GUID is ever hardcoded here. A mistyped GUID is invisible, a
// handler name that does not match is not.
[[nodiscard]] std::string sevenzip_handler_for(const std::filesystem::path &archive);

// Extract `archive` into `dest_dir` with the 7-Zip backend.
//
// `ask_password` is called on the calling thread whenever 7-Zip needs a
// decryption key, and returns the password to try, or an empty string to
// refuse. It is wired to the shared PassphraseSession, so this path spends the
// same attempt budget as the libarchive path and a dismissal is a cancel
// rather than a failure. The password is handed to 7-Zip as a BSTR and
// nowhere else: never a command line, an environment entry or a temp file.
//
// `on_progress` receives bytes written against the archive's total, the same
// done/total contract ExtractProgressFn describes.
//
// Returns false on failure with a human-readable reason in `error`, and leaves
// nothing outside `dest_dir` behind.
bool extract_with_sevenzip(const std::filesystem::path &archive,
                           const std::filesystem::path &dest_dir,
                           std::vector<ExtractedFile> &out_files, std::string &error,
                           const ExtractProgressFn &on_progress,
                           const std::function<bool(const std::string &archive_name,
                                                    std::string &passphrase)> &ask_password);

}  // namespace engine
