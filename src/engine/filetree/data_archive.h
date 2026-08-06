#pragma once

// Read-only archive listing seam (PLAN §19.3.1's "DataArchive backend").
// A DataArchive is a source of named, typed entries that a FileTree can be
// built from. libarchive provides the implementation today (zip/7z/tar/rar/
// gz/bz2/xz); BSA/BA2 backends plug in here later. Qt-free.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

// One entry in an archive's listing (MO2's FileData analogue).
struct ArchiveEntryInfo {
    std::string path;  // '/'-separated, relative to the archive root, no leading slash
    bool is_dir = false;
    int64_t size = 0;    // uncompressed size (files only)
    int64_t index = -1;  // ordinal in the listing, for future selected extraction
};

// A read-only archive source that can enumerate its entries.
class DataArchive {
public:
    virtual ~DataArchive() = default;

    // Fill `out` with every entry. Returns true on success; on failure `error`
    // (when non-null) holds a human-readable reason.
    virtual bool list(std::vector<ArchiveEntryInfo>& out,
                      std::string* error = nullptr) const = 0;
};

// libarchive-backed DataArchive. Paths are normalized to '/'-separated
// Windows-native form and entries whose names are "." or ".." are dropped,
// mirroring MO2's ArchiveFileTree::makeTree.
[[nodiscard]] std::shared_ptr<DataArchive> open_libarchive(
    const std::filesystem::path& archive);

}  // namespace engine
