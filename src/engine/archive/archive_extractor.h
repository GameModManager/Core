#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

struct ExtractedFile {
    std::string archive_path;
    std::filesystem::path dest_path;
};

class ArchiveExtractor {
public:
    // Extract any supported archive (.zip, .7z, .tar, .rar, .gz, .bz2, .xz)
    // to dest_dir. Returns true on success; on failure `error` holds a
    // human-readable reason (libarchive diagnostics + filesystem errors) so
    // the caller can log *why* extraction failed.
    static bool extract(const std::filesystem::path& archive,
                        const std::filesystem::path& dest_dir,
                        std::vector<ExtractedFile>& out_files,
                        std::string& error);
};

}  // namespace engine
