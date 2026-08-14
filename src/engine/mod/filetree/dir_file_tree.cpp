#include "engine/mod/filetree/dir_file_tree.h"

#include "engine/core/util/fs_utils.h"

#include <memory>
#include <system_error>

namespace engine {

DirectoryFileTree::DirectoryFileTree(std::shared_ptr<const FileTree> parent,
                                     std::string name, NameCompare cmp,
                                     std::filesystem::path path,
                                     bool ignore_meta_ini)
    : FileTree(std::move(parent), std::move(name), cmp),
      m_path(std::move(path)),
      m_ignore_meta_ini(ignore_meta_ini) {}

std::shared_ptr<FileTree> DirectoryFileTree::make_tree(
    const std::filesystem::path& dir, NameCompare cmp, bool ignore_meta_ini) {
    return std::make_shared<DirectoryFileTree>(nullptr, dir.filename().string(), cmp,
                                               dir, ignore_meta_ini);
}

bool DirectoryFileTree::do_populate(std::shared_ptr<const FileTree> parent,
                                    std::vector<value_type>& out) const {
    std::error_code ec;
    const std::filesystem::directory_iterator end;
    for (auto it = std::filesystem::directory_iterator(m_path, ec); it != end;
         it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        const std::string name = p.filename().string();
        const bool is_dir = it->is_directory(ec);
        if (is_dir) {
            out.push_back(std::make_shared<DirectoryFileTree>(parent, name, m_cmp, p,
                                                              m_ignore_meta_ini));
        } else {
            // MO2 QDirRootFileTreeImpl: hide the mod's own meta.ini at the root
            // only (a meta.ini in a subdirectory is mod content).
            if (m_ignore_meta_ini && name_equals(name, "meta.ini", m_cmp)) continue;
            out.push_back(std::make_shared<FileTreeEntry>(parent, name, m_cmp));
        }
    }
    // std::filesystem iteration order is unspecified - let the base sort.
    return false;
}

}  // namespace engine
