#pragma once

// Disk-backed file tree (MO2 QDirFileTree): each node is a directory on disk,
// lazily enumerated on first access. Read-only - the tree mirrors the disk,
// never mutates it. Qt-free.

#include <filesystem>
#include <string>

#include "engine/filetree/file_tree.h"

namespace engine {

class DirectoryFileTree final : public FileTree {
public:
    // Tree over `dir`. When `ignore_meta_ini` is set, a root-level meta.ini
    // file is hidden (MO2's ignoreRootMeta, used for mod-folder roots).
    static std::shared_ptr<FileTree> make_tree(
        const std::filesystem::path& dir,
        NameCompare cmp = NameCompare::CaseInsensitive, bool ignore_meta_ini = true);

    // The on-disk directory this node represents.
    const std::filesystem::path& on_disk_path() const { return m_path; }

    DirectoryFileTree(std::shared_ptr<const FileTree> parent, std::string name,
                      NameCompare cmp, std::filesystem::path path,
                      bool ignore_meta_ini);

protected:
    bool do_populate(std::shared_ptr<const FileTree> parent,
                     std::vector<value_type>& out) const override;

private:
    std::filesystem::path m_path;
    bool m_ignore_meta_ini = true;
};

}  // namespace engine
