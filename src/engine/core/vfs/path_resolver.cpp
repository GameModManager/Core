#include "path_resolver.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace engine::vfs {

namespace {

// Lowercase a copy of the string.
[[nodiscard]] std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// Translate Windows backslash separators to '/'.
[[nodiscard]] std::string normalize_separators(std::string p) {
  std::replace(p.begin(), p.end(), '\\', '/');
  return p;
}

// True when any path component is ".." (a directory-escape attempt).
[[nodiscard]] bool has_parent_escape(std::string_view rel) {
  const std::filesystem::path p(rel);
  return std::any_of(p.begin(), p.end(), [](const std::filesystem::path &part) {
    return part == "..";
  });
}

} // namespace

struct PathResolver::Impl {
  std::filesystem::path root;
  NameCompare cmp = NameCompare::CaseInsensitive;

  // Cache: directory CI-key -> on-disk entry names. Populated lazily as
  // resolve()/list() walk the tree (IndexedBackend only). On a natively
  // case-insensitive filesystem (Windows) resolution is lexical and never
  // scans, so this cache stays empty there.
  mutable std::unordered_map<std::string, std::vector<std::string>> dir_index;
  mutable std::shared_mutex index_mu;
};

PathResolver::PathResolver(std::filesystem::path root, NameCompare cmp)
    : impl_(std::make_unique<Impl>()) {
  impl_->root = std::move(root);
  impl_->cmp = cmp;
}

PathResolver::~PathResolver() = default;
PathResolver::PathResolver(PathResolver &&) noexcept = default;
PathResolver &PathResolver::operator=(PathResolver &&) noexcept = default;

const std::filesystem::path &PathResolver::root() const { return impl_->root; }

bool PathResolver::is_native_ci() const {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

std::string PathResolver::normalize(std::string_view game_rel) const {
  std::string p = normalize_separators(std::string(game_rel));
  if (impl_->cmp == NameCompare::CaseInsensitive) {
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
  }
  return p;
}

// Walk `rel` (already separator-normalized, non-absolute, no "..") to its real
// on-disk absolute path, matching each component case-insensitively against the
// tree and caching every directory scanned. Returns nullopt when a component is
// not found. On Windows this is a lexical normalize + exists() with no scan.
std::optional<std::filesystem::path>
PathResolver::walk_to_absolute(PathResolver::Impl &self,
                               const std::string &rel) {
#ifdef _WIN32
  std::error_code ec;
  const auto candidate =
      (self.root / std::filesystem::path(rel)).lexically_normal();
  if (!std::filesystem::exists(candidate, ec)) {
    return std::nullopt;
  }
  return candidate;
#else
  // Writes the cache, so a unique lock for the whole walk. Reads (resolve/
  // exists/list/normalize) that only consult the cache use shared_lock; the
  // cache-populating walk is the one write path and is internally serialized.
  std::unique_lock<std::shared_mutex> lock(self.index_mu);

  std::filesystem::path cur = self.root;
  std::string rel_acc; // original-cased accumulation, for stable cache keys
  for (const auto &part : std::filesystem::path(rel)) {
    std::string comp = part.string();
    if (comp.empty() || comp == ".") {
      continue;
    }
    const std::string dir_key = [&] {
      std::string k = normalize_separators(rel_acc);
      if (self.cmp == NameCompare::CaseInsensitive) {
        k = to_lower(std::move(k));
      }
      return k;
    }();

    std::error_code ec;
    std::vector<std::string> entries;
    bool found = false;
    std::filesystem::path match;
    const bool ci = self.cmp == NameCompare::CaseInsensitive;
    for (const auto &e : std::filesystem::directory_iterator(cur, ec)) {
      if (ec) {
        break;
      }
      const std::string name = e.path().filename().string();
      entries.push_back(name);
      if (!found &&
          (name == comp || (ci && to_lower(name) == to_lower(comp)))) {
        match = e.path();
        found = true;
      }
    }
    self.dir_index[dir_key] = std::move(entries);
    if (!found) {
      return std::nullopt;
    }
    cur = match;
    if (!rel_acc.empty()) {
      rel_acc += '/';
    }
    rel_acc += comp;
  }
  return cur;
#endif
}

std::optional<GameFile> PathResolver::resolve(std::string_view game_rel) const {
  const std::string rel = normalize_separators(std::string(game_rel));
  if (rel.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path relp(rel);
  if (relp.is_absolute()) {
    return std::nullopt;
  }
  if (has_parent_escape(rel)) {
    return std::nullopt;
  }
  const auto abs = PathResolver::walk_to_absolute(*impl_, rel);
  if (!abs) {
    return std::nullopt;
  }
  return GameFile{*abs, normalize(game_rel), std::string(game_rel)};
}

bool PathResolver::exists(std::string_view game_rel) const {
  const std::string rel = normalize_separators(std::string(game_rel));
  if (rel.empty()) {
    return false;
  }
  const std::filesystem::path relp(rel);
  if (relp.is_absolute()) {
    return false;
  }
  if (has_parent_escape(rel)) {
    return false;
  }
  return PathResolver::walk_to_absolute(*impl_, rel).has_value();
}

std::vector<GameFile> PathResolver::list(std::string_view dir_rel) const {
  const std::string rel = normalize_separators(std::string(dir_rel));
  if (!rel.empty()) {
    const std::filesystem::path relp(rel);
    if (relp.is_absolute()) {
      return {};
    }
    if (has_parent_escape(rel)) {
      return {};
    }
  }

  const auto abs_dir = PathResolver::walk_to_absolute(*impl_, rel);
  if (!abs_dir) {
    return {};
  }

  const std::string dir_key = normalize(dir_rel);
  std::vector<GameFile> out;
  {
    std::shared_lock<std::shared_mutex> lock(impl_->index_mu);
    const auto it = impl_->dir_index.find(dir_key);
    if (it == impl_->dir_index.end()) {
      // Not cached (e.g. listing the root, which walk_to_absolute returns
      // without scanning). Fall back to a direct scan; correctness over
      // caching here.
      lock.unlock();
      std::error_code ec;
      for (const auto &e : std::filesystem::directory_iterator(*abs_dir, ec)) {
        if (ec) {
          break;
        }
        const std::string name = e.path().filename().string();
        const std::string entry_rel = rel.empty() ? name : (rel + "/" + name);
        out.emplace_back(GameFile{e.path(), normalize(entry_rel), entry_rel});
      }
      return out;
    }
    for (const auto &name : it->second) {
      const std::string entry_rel = rel.empty() ? name : (rel + "/" + name);
      out.emplace_back(
          GameFile{*abs_dir / name, normalize(entry_rel), entry_rel});
    }
  }
  return out;
}

void PathResolver::invalidate(std::string_view game_rel) {
  const std::string p = normalize_separators(std::string(game_rel));
  std::unique_lock<std::shared_mutex> lock(impl_->index_mu);
  impl_->dir_index.erase(normalize(p));
  const std::filesystem::path pp(p);
  if (pp.has_parent_path()) {
    impl_->dir_index.erase(normalize(pp.parent_path().generic_string()));
  }
}

void PathResolver::invalidate_all() {
  std::unique_lock<std::shared_mutex> lock(impl_->index_mu);
  impl_->dir_index.clear();
}

} // namespace engine::vfs
