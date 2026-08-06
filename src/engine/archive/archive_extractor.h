#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine {

struct ExtractedFile {
    std::string archive_path;
    std::filesystem::path dest_path;
};

// Install-time extraction progress. Invoked with the bytes written so far and
// the archive's total uncompressed size (summed in a cheap header pre-pass, so
// the bar shows a real percent instead of a busy spinner). `total <= 0` means
// the size could not be determined - the caller should show an indeterminate
// bar. Qt-free; the UI wires it to its progress dialog via PipelineContext.
using ExtractProgressFn = std::function<void(int64_t done, int64_t total)>;

class ArchiveExtractor {
public:
    // Extract any supported archive (.zip, .7z, .tar, .rar, .gz, .bz2, .xz)
    // to dest_dir. Returns true on success; on failure `error` holds a
    // human-readable reason (libarchive diagnostics + filesystem errors) so
    // the caller can log *why* extraction failed.
    // `on_progress`, when set, reports done/total bytes (see ExtractProgressFn);
    // it is invoked on the calling thread and is throttled to avoid flooding
    // the UI on archives with many small files.
    static bool extract(const std::filesystem::path& archive,
                        const std::filesystem::path& dest_dir,
                        std::vector<ExtractedFile>& out_files,
                        std::string& error,
                        const ExtractProgressFn& on_progress = {});
};

// True when `archive` carries a RAR signature (RAR4 "Rar!\x1a\x07\x00" or
// RAR5 "Rar!\x1a\x07\x01\x00"). Used by extract() to decide whether the unrar
// CLI fallback may help when libarchive's RAR reader rejects the archive.
[[nodiscard]] bool is_rar_archive(const std::filesystem::path& archive);

}  // namespace engine
