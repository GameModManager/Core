#pragma once

// libarchive-backed file tree (MO2's ArchiveFileTree): a directory node over a
// flat archive listing. The root opens and lists the archive once (eagerly, in
// make_tree, so open/list failures surface there); each directory node lazily
// groups its children from that shared flat list on first access. Read-only.
// Qt-free.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/filetree/data_archive.h"
#include "engine/filetree/file_tree.h"

namespace engine {

class ArchiveFileTree final : public FileTree {
public:
    // Tree over any libarchive-supported archive (zip/7z/tar/rar/gz/bz2/xz).
    // Returns null on open/list failure with `error` set to a human-readable
    // reason.
    static std::shared_ptr<FileTree> make_tree(const std::filesystem::path& archive,
                                               std::string* error,
                                               NameCompare cmp);

    // The archive this tree represents.
    const std::filesystem::path& archive_path() const { return m_archive; }

    ArchiveFileTree(std::shared_ptr<const FileTree> parent, std::string name,
                    NameCompare cmp, std::filesystem::path archive,
                    std::string prefix,
                    std::shared_ptr<const std::vector<ArchiveEntryInfo>> entries);

protected:
    bool do_populate(std::shared_ptr<const FileTree> parent,
                     std::vector<value_type>& out) const override;

private:
    std::filesystem::path m_archive;
    std::string m_prefix;  // this node's path within the archive; "" at the root
    std::shared_ptr<const std::vector<ArchiveEntryInfo>> m_entries;
};

}  // namespace engine
