#include "engine/filetree/file_tree.h"

#include "engine/filetree/archive_file_tree.h"
#include "engine/filetree/dir_file_tree.h"
#include "engine/util/fs_utils.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace engine {

int name_compare(const std::string& a, const std::string& b, NameCompare cmp) {
    if (cmp == NameCompare::CaseSensitive) {
        if (a < b) return -1;
        if (a > b) return 1;
        return 0;
    }
    const std::string la = toLower(a);
    const std::string lb = toLower(b);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

bool name_equals(const std::string& a, const std::string& b, NameCompare cmp) {
    if (cmp == NameCompare::CaseSensitive) return a == b;
    return toLower(a) == toLower(b);
}

FileTreeEntry::FileTreeEntry(std::shared_ptr<const FileTree> parent,
                             std::string name, NameCompare cmp)
    : m_name(std::move(name)), m_cmp(cmp), m_parent(std::move(parent)) {}

int FileTreeEntry::compare(const std::string& name) const {
    return name_compare(m_name, name, m_cmp);
}

std::string FileTreeEntry::suffix() const {
    if (is_dir()) return {};
    const auto dot = m_name.find_last_of('.');
    if (dot == std::string::npos) return {};
    return m_name.substr(dot + 1);
}

bool FileTreeEntry::has_suffix(const std::string& suffix) const {
    return is_file() && name_equals(this->suffix(), suffix, m_cmp);
}

std::string FileTreeEntry::path(std::string_view sep) const {
    return path_from(nullptr, sep);
}

std::string FileTreeEntry::path_from(std::shared_ptr<const FileTree> tree,
                                     std::string_view sep) const {
    std::string result = m_name;
    std::shared_ptr<const FileTree> p = parent();
    while (p != nullptr && p != tree) {
        result = p->name().empty() ? result : p->name() + std::string(sep) + result;
        p = p->parent();
    }
    // tree non-null but never reached: it is not an ancestor of this entry.
    if (tree != nullptr && p != tree) return {};
    return result;
}

std::shared_ptr<FileTree> FileTreeEntry::parent() {
    return std::const_pointer_cast<FileTree>(m_parent.lock());
}

std::shared_ptr<const FileTree> FileTreeEntry::parent() const {
    return m_parent.lock();
}

FileTree::FileTree(std::string name, NameCompare cmp)
    : FileTreeEntry(nullptr, std::move(name), cmp) {}

FileTree::FileTree(std::shared_ptr<const FileTree> parent, std::string name,
                   NameCompare cmp)
    : FileTreeEntry(std::move(parent), std::move(name), cmp) {}

std::shared_ptr<FileTree> FileTree::make_tree_from_directory(
    const std::filesystem::path& dir, NameCompare cmp, bool ignore_meta_ini) {
    return DirectoryFileTree::make_tree(dir, cmp, ignore_meta_ini);
}

std::shared_ptr<FileTree> FileTree::make_tree_from_archive(
    const std::filesystem::path& archive, std::string* error, NameCompare cmp) {
    return ArchiveFileTree::make_tree(archive, error, cmp);
}

const std::vector<FileTree::value_type>& FileTree::entries() const {
    if (!m_populated.load(std::memory_order_acquire)) populate();
    return m_entries;
}

void FileTree::populate() const {
    std::call_once(m_once, [this] {
        std::vector<value_type> out;
        auto self = std::dynamic_pointer_cast<const FileTree>(shared_from_this());
        if (!do_populate(self, out)) {
            // Sort directories-first, then by name under this tree's
            // comparator (the order MO2's install code iterates).
            std::stable_sort(out.begin(), out.end(),
                             [this](const value_type& a, const value_type& b) {
                                 if (a->is_dir() != b->is_dir()) return a->is_dir();
                                 return name_compare(a->name(), b->name(), m_cmp) < 0;
                             });
        }
        m_entries = std::move(out);
        m_populated.store(true, std::memory_order_release);
    });
}

std::shared_ptr<FileTreeEntry> FileTree::at(std::size_t i) {
    auto& v = entries();
    if (i >= v.size()) throw std::out_of_range("FileTree::at");
    return v[i];
}

std::shared_ptr<const FileTreeEntry> FileTree::at(std::size_t i) const {
    const auto& v = entries();
    if (i >= v.size()) throw std::out_of_range("FileTree::at");
    return v[i];
}

bool FileTree::exists(const std::string& path) const {
    return find(path) != nullptr;
}

std::shared_ptr<FileTreeEntry> FileTree::find(const std::string& path) {
    return std::const_pointer_cast<FileTreeEntry>(std::as_const(*this).find(path));
}

std::shared_ptr<const FileTreeEntry> FileTree::find(const std::string& path) const {
    // Split on both separators, dropping empty parts (MO2 splitPath).
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return {};

    const FileTree* node = this;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i] == ".") continue;
        if (parts[i] == "..") return {};  // no traversal
        auto next = node->find_directory(parts[i]);
        if (!next) return {};
        node = next.get();
    }
    const std::string& last = parts.back();
    if (last == "." || last == "..") return {};
    for (const auto& e : node->entries()) {
        if (name_equals(e->name(), last, node->m_cmp)) return e;
    }
    return {};
}

std::shared_ptr<FileTree> FileTree::find_directory(const std::string& path) {
    return std::const_pointer_cast<FileTree>(std::as_const(*this).find_directory(path));
}

std::shared_ptr<const FileTree> FileTree::find_directory(
    const std::string& path) const {
    auto entry = find(path);
    return (entry != nullptr && entry->is_dir()) ? entry->as_tree() : nullptr;
}

std::string FileTree::path_to(const const_reference& entry,
                              std::string_view sep) const {
    auto self = std::dynamic_pointer_cast<const FileTree>(shared_from_this());
    return entry->path_from(self, sep);
}

namespace {

// Returns false when the walk should stop (CONTINUE/STOP propagation).
bool walk_node(std::shared_ptr<const FileTree> tree, const std::string& prefix,
               const std::function<FileTree::WalkReturn(
                   const std::string&, const FileTree::const_reference&)>& cb,
               std::string_view sep) {
    for (auto entry : *tree) {
        const FileTree::WalkReturn r = cb(prefix, entry);
        if (r == FileTree::WalkReturn::Stop) return false;
        if (r == FileTree::WalkReturn::Skip) continue;
        if (entry->is_dir()) {
            if (!walk_node(entry->as_tree(), prefix + entry->name() + std::string(sep),
                           cb, sep)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

void FileTree::walk(
    const std::function<WalkReturn(const std::string&, const const_reference&)>& cb,
    std::string_view sep) const {
    auto self = std::dynamic_pointer_cast<const FileTree>(shared_from_this());
    walk_node(self, "", cb, sep);
}

}  // namespace engine
