#include "engine/mod/filetree/archive_file_tree.h"

#include <utility>

namespace engine {

namespace {

// Linear child lookup by name (children count is small). Case-insensitive
// when the tree's comparator is, matching the base sort's ordering.
bool contains_child(const std::vector<FileTree::value_type>& children,
                    const std::string& name, NameCompare cmp) {
    for (const auto& child : children) {
        if (name_equals(child->name(), name, cmp)) return true;
    }
    return false;
}

}  // namespace

ArchiveFileTree::ArchiveFileTree(std::shared_ptr<const FileTree> parent,
                                 std::string name, NameCompare cmp,
                                 std::filesystem::path archive, std::string prefix,
                                 std::shared_ptr<const std::vector<ArchiveEntryInfo>> entries)
    : FileTree(std::move(parent), std::move(name), cmp),
      m_archive(std::move(archive)),
      m_prefix(std::move(prefix)),
      m_entries(std::move(entries)) {}

std::shared_ptr<FileTree> ArchiveFileTree::make_tree(const std::filesystem::path& archive,
                                                     std::string* error, NameCompare cmp) {
    auto arch = open_libarchive(archive);
    if (!arch) {
        if (error) *error = "no archive backend for " + archive.string();
        return nullptr;
    }
    auto entries = std::make_shared<std::vector<ArchiveEntryInfo>>();
    std::string list_error;
    if (!arch->list(*entries, &list_error)) {
        if (error) *error = list_error;
        return nullptr;
    }
    return std::make_shared<ArchiveFileTree>(nullptr, archive.filename().string(), cmp,
                                             archive, "", std::move(entries));
}

bool ArchiveFileTree::do_populate(std::shared_ptr<const FileTree> parent,
                                  std::vector<value_type>& out) const {
    const std::size_t plen = m_prefix.size();
    const std::string base = m_prefix.empty() ? "" : m_prefix + "/";
    for (const auto& info : *m_entries) {
        const std::string& path = info.path;
        if (!m_prefix.empty()) {
            if (path.size() <= plen || path.compare(0, plen, m_prefix) != 0 ||
                path[plen] != '/') {
                continue;
            }
        }
        const std::string rel = m_prefix.empty() ? path : path.substr(plen + 1);
        const std::size_t slash = rel.find('/');
        const std::string first = slash == std::string::npos ? rel : rel.substr(0, slash);
        if (first.empty()) continue;

        if (slash != std::string::npos) {
            // A file deeper inside this folder: the first component is a
            // (possibly implicit) subdirectory.
            if (!contains_child(out, first, m_cmp)) {
                out.push_back(std::make_shared<ArchiveFileTree>(parent, first, m_cmp,
                                                                m_archive, base + first,
                                                                m_entries));
            }
        } else if (info.is_dir) {
            if (!contains_child(out, first, m_cmp)) {
                out.push_back(std::make_shared<ArchiveFileTree>(parent, first, m_cmp,
                                                                m_archive, base + first,
                                                                m_entries));
            }
        } else {
            if (!contains_child(out, first, m_cmp)) {
                out.push_back(std::make_shared<FileTreeEntry>(parent, first, m_cmp));
            }
        }
    }
    // Grouping visits the flat list in archive order - let the base sort
    // (directories first, then names under the tree's comparator).
    return false;
}

}  // namespace engine
