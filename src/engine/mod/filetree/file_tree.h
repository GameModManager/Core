#pragma once

// Qt-free unified read-only file tree (PLAN §19.4 P1.1). Modeled on MO2's
// IFileTree / FileTreeEntry (REFERENCES/modorganizer-preview_nif/mo2-abi/
// 2.5.2/include/uibase/ifiletree.h): a "virtual" tree that may represent a
// folder on disk, an archive, or (later) a merged data view, with lazy
// per-node population and the same entry/tree relationship - a directory entry
// IS a tree (as_tree()).
//
// Read-only in this slice: a tree can be enumerated, searched and walked but
// not edited in-memory. In-memory mutations (insert/merge - MO2's mutable
// surface) are deferred until an in-tree installer needs them.
//
// Engine rule: no Qt includes here.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// Case-sensitivity of name comparisons for a tree. MO2's FileNameComparator is
// always case-insensitive (Windows filenames); GMM carries a per-game
// case_sensitive knowledge hook, so the comparator is selectable per tree.
enum class NameCompare {
    CaseInsensitive,  // default - matches MO2 and Windows-authored archives
    CaseSensitive,
};

// Compare two file names under the given mode. Returns -1, 0 or 1.
[[nodiscard]] int name_compare(const std::string& a, const std::string& b,
                               NameCompare cmp);
[[nodiscard]] bool name_equals(const std::string& a, const std::string& b,
                               NameCompare cmp);

class FileTree;

// A node in a file tree: either a file or a directory. A directory node IS a
// FileTree (as_tree()); a file's as_tree() is null. Down-links are strong
// shared pointers, up-links (parent) are weak: as long as the root lives,
// entries stay alive; a detached entry loses its parent chain.
class FileTreeEntry : public std::enable_shared_from_this<FileTreeEntry> {
public:
    virtual ~FileTreeEntry() = default;

    // This entry as a tree (directory entries), or null for files.
    virtual std::shared_ptr<FileTree> as_tree() { return nullptr; }
    virtual std::shared_ptr<const FileTree> as_tree() const { return nullptr; }

    bool is_file() const { return as_tree() == nullptr; }
    bool is_dir() const { return as_tree() != nullptr; }

    const std::string& name() const { return m_name; }

    // Compare this entry's name against `name` under this entry's comparator.
    // Returns -1, 0 or 1 (name only, not the full path).
    int compare(const std::string& name) const;

    // Last extension (everything after the last '.'); empty for directories
    // and extension-less files.
    std::string suffix() const;

    // True when this entry is a file with the given suffix (case-insensitive).
    bool has_suffix(const std::string& suffix) const;

    // Path from the root of the tree to this entry, including this entry's
    // name (MO2 FileTreeEntry::path). O(depth), the path is never stored.
    std::string path(std::string_view sep = "/") const;

    // Path from `tree` (an ancestor of this entry) to this entry, including
    // this entry's name. Empty when `tree` is not an ancestor; a null tree
    // means "from the root".
    std::string path_from(std::shared_ptr<const FileTree> tree,
                          std::string_view sep = "/") const;

    // The immediate parent tree of this entry (null for the root).
    std::shared_ptr<FileTree> parent();
    std::shared_ptr<const FileTree> parent() const;

    // Backends (dir/archive trees) create file leaves directly; copy/move are
    // disallowed - entries live in trees only.
    FileTreeEntry(std::shared_ptr<const FileTree> parent, std::string name,
                  NameCompare cmp);
    FileTreeEntry(FileTreeEntry const&) = delete;
    FileTreeEntry(FileTreeEntry&&) = delete;
    FileTreeEntry& operator=(FileTreeEntry const&) = delete;
    FileTreeEntry& operator=(FileTreeEntry&&) = delete;

protected:
    std::string m_name;
    NameCompare m_cmp = NameCompare::CaseInsensitive;
    std::weak_ptr<const FileTree> m_parent;

    friend class FileTree;
};

// A directory in a file tree. Population is lazy and thread-safe: the first
// read of a node enumerates its children (std::call_once), so building a tree
// is cheap and only the subtrees actually visited touch their source. Read-only
// operations are safe before population, matching MO2's contract.
//
// Directory children are themselves FileTree objects; file children are plain
// FileTreeEntry (or a backend subclass carrying extra data, e.g. the archive
// index). Children of a node are kept sorted directories-first, then by name
// case-insensitively (the sort MO2's installer code expects).
class FileTree : public FileTreeEntry {
public:
    using value_type = std::shared_ptr<FileTreeEntry>;
    using const_reference = std::shared_ptr<const FileTreeEntry>;
    using iterator = std::vector<value_type>::const_iterator;

    // A directory entry IS a tree: return this node as a tree.
    std::shared_ptr<FileTree> as_tree() override {
        return std::dynamic_pointer_cast<FileTree>(shared_from_this());
    }
    std::shared_ptr<const FileTree> as_tree() const override {
        return std::dynamic_pointer_cast<const FileTree>(shared_from_this());
    }

    // const_iterator converts the stored shared_ptr<FileTreeEntry> into
    // shared_ptr<const FileTreeEntry>, so iterating a const tree yields
    // immutable entries (MO2's convert_iterator).
    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = const_reference;
        using difference_type = std::ptrdiff_t;
        using pointer = const_reference;
        using reference = const_reference;

        const_iterator() = default;
        const_iterator(const_iterator const&) = default;
        const_iterator(const_iterator&&) = default;
        const_iterator& operator=(const_iterator const&) = default;
        const_iterator& operator=(const_iterator&&) = default;

        reference operator*() const { return {*v}; }
        reference operator->() const { return {*v}; }
        const_iterator& operator++() {
            ++v;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator copy = *this;
            ++v;
            return copy;
        }
        friend bool operator==(const_iterator a, const_iterator b) {
            return a.v == b.v;
        }
        friend bool operator!=(const_iterator a, const_iterator b) {
            return a.v != b.v;
        }

    private:
        friend class FileTree;
        // Takes the vector's own const_iterator (the iterator typedef), so no
        // implicit conversions are involved.
        const_iterator(iterator v) : v(v) {}
        iterator v;
    };

    // — factories ------------------------------------------------------------
    // Tree over a directory on disk (MO2 QDirFileTree). Lazily enumerated;
    // when `ignore_meta_ini` is set, a root-level meta.ini is hidden (MO2's
    // ignoreRootMeta for mod-folder roots; subdirectory meta.ini is kept).
    static std::shared_ptr<FileTree> make_tree_from_directory(
        const std::filesystem::path& dir,
        NameCompare cmp = NameCompare::CaseInsensitive, bool ignore_meta_ini = true);

    // Tree over any libarchive-supported archive (zip/7z/tar/rar/gz/bz2/xz)
    // (MO2 ArchiveFileTree). Returns null on open/list failure with `error`
    // set to a human-readable reason.
    static std::shared_ptr<FileTree> make_tree_from_archive(
        const std::filesystem::path& archive, std::string* error = nullptr,
        NameCompare cmp = NameCompare::CaseInsensitive);

    // — iteration ------------------------------------------------------------
    iterator begin() { return {std::cbegin(entries())}; }
    const_iterator begin() const { return {std::cbegin(entries())}; }
    const_iterator cbegin() const { return {std::cbegin(entries())}; }
    iterator end() { return {std::cend(entries())}; }
    const_iterator end() const { return {std::cend(entries())}; }
    const_iterator cend() const { return {std::cend(entries())}; }

    // Number of direct children. Constant after population.
    std::size_t size() const { return entries().size(); }
    bool empty() const { return size() == 0; }

    // Child at index `i`. Throws std::out_of_range for an invalid index.
    value_type at(std::size_t i);
    const_reference at(std::size_t i) const;

    // — lookup ---------------------------------------------------------------
    // Paths are '/'- or '\\'-separated (Windows-native archive paths).
    bool exists(const std::string& path) const;
    value_type find(const std::string& path);
    const_reference find(const std::string& path) const;

    // find() restricted to directories, returned as a tree.
    std::shared_ptr<FileTree> find_directory(const std::string& path);
    std::shared_ptr<const FileTree> find_directory(const std::string& path) const;

    // Path from this tree to the given entry (including the entry name), or
    // empty when the entry does not belong to this tree.
    std::string path_to(const const_reference& entry,
                        std::string_view sep = "/") const;

    // — walking --------------------------------------------------------------
    enum class WalkReturn {
        // Continue walking normally.
        Continue,
        // Stop the whole walk.
        Stop,
        // Skip this folder (no effect on files).
        Skip,
    };

    // Depth-first walk; parents are always visited before their children, the
    // root itself is never visited. The callback receives the path from this
    // tree to the entry's parent (with a trailing separator, empty for direct
    // children) and the entry; its return value steers the walk.
    void walk(
        const std::function<WalkReturn(const std::string& prefix,
                                       const const_reference& entry)>& callback,
        std::string_view sep = "/") const;

protected:
    // Root node (no parent).
    FileTree(std::string name, NameCompare cmp);
    // Child node under `parent`.
    FileTree(std::shared_ptr<const FileTree> parent, std::string name,
             NameCompare cmp);
    FileTree(FileTree const&) = delete;
    FileTree(FileTree&&) = delete;
    FileTree& operator=(FileTree const&) = delete;
    FileTree& operator=(FileTree&&) = delete;

    // Populate `out` with this node's children. `parent` is this node, passed
    // so backends can construct children with the right parent link. Return
    // true when `out` is already sorted (directories first, then names
    // case-insensitively); false lets the base sort it. Must not throw.
    virtual bool do_populate(std::shared_ptr<const FileTree> parent,
                             std::vector<value_type>& out) const = 0;

    const std::vector<value_type>& entries() const;  // triggers population

private:
    void populate() const;

    mutable std::atomic<bool> m_populated{false};
    mutable std::once_flag m_once;
    mutable std::vector<value_type> m_entries;
};

}  // namespace engine
