#include "engine/core/instance/instance_registry.h"

#include "engine/core/instance/toml_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/util/fs_utils.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "platform/platform.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <toml++/toml.hpp>

namespace fs = std::filesystem;

namespace engine {

namespace {
constexpr int kRegistryVersion = 1;
} // namespace

// --- Entry ---

bool InstanceRegistry::Entry::exists() const {
  std::error_code ec;
  fs::path root_path(root);
  if (!fs::is_directory(root_path, ec))
    return false;
  return fs::exists(root_path / "instance.toml", ec);
}

// --- Construction / Destruction ---

InstanceRegistry::InstanceRegistry() : path_(registry_path()) { (void)load(); }

InstanceRegistry::~InstanceRegistry() = default;

// --- Persistence ---

fs::path InstanceRegistry::registry_path() {
  return safe_home_dir() / ".config" / "GameModManager" /
         "instance_registry.toml";
}

bool InstanceRegistry::load() {
  std::error_code ec;
  if (!fs::exists(path_, ec))
    return true; // empty registry is fine

  std::ifstream in(path_);
  if (!in) {
    Logger::instance().warn("InstanceRegistry: cannot open " + path_.string());
    return false;
  }

  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());

  toml::table tbl;
  try {
    tbl = toml::parse(content);
  } catch (const toml::parse_error &err) {
    Logger::instance().error("InstanceRegistry: parse error: " +
                             std::string(err.what()));
    return false;
  }

  // Check version
  auto version = tbl["version"].value<int>();
  if (!version) {
    Logger::instance().warn("InstanceRegistry: missing version field");
    return false;
  }
  if (*version > kRegistryVersion) {
    Logger::instance().warn("InstanceRegistry: unsupported version " +
                            std::to_string(*version));
    return false;
  }

  entries_.clear();
  auto instances = tbl["instances"].as_array();
  if (!instances)
    return true; // no instances yet

  for (const auto &item : *instances) {
    auto *tbl_item = item.as_table();
    if (!tbl_item)
      continue;

    Entry entry;
    if (auto v = (*tbl_item)["name"].value<std::string>())
      entry.name = *v;
    if (auto v = (*tbl_item)["root"].value<std::string>())
      entry.root = *v;
    if (auto v = (*tbl_item)["type"].value<std::string>())
      entry.type = *v;
    if (auto v = (*tbl_item)["game_id"].value<std::string>())
      entry.game_id = *v;
    if (auto v = (*tbl_item)["display_name"].value<std::string>())
      entry.display_name = *v;
    if (auto v = (*tbl_item)["created_at"].value<std::string>())
      entry.created_at = *v;
    if (auto v = (*tbl_item)["last_used_at"].value<std::string>())
      entry.last_used_at = *v;
    if (auto v = (*tbl_item)["is_active"].value<bool>())
      entry.is_active = *v;

    if (!entry.name.empty())
      entries_.push_back(std::move(entry));
  }

  Logger::instance().debug("InstanceRegistry: loaded " +
                           std::to_string(entries_.size()) + " entries");
  return true;
}

bool InstanceRegistry::save() const {
  toml::table tbl;
  tbl.emplace("version", kRegistryVersion);

  toml::array instances_arr;
  for (const auto &e : entries_) {
    toml::table entry_tbl;
    entry_tbl.emplace("name", e.name);
    entry_tbl.emplace("root", e.root);
    entry_tbl.emplace("type", e.type);
    entry_tbl.emplace("game_id", e.game_id);
    entry_tbl.emplace("display_name", e.display_name);
    entry_tbl.emplace("created_at", e.created_at);
    entry_tbl.emplace("last_used_at", e.last_used_at);
    entry_tbl.emplace("is_active", e.is_active);
    instances_arr.push_back(std::move(entry_tbl));
  }
  tbl.emplace("instances", std::move(instances_arr));

  // Write atomically: write to .tmp, then rename
  auto tmp_path = path_.string() + ".tmp";
  std::ofstream out(tmp_path);
  if (!out) {
    Logger::instance().error("InstanceRegistry: cannot write to " + tmp_path);
    return false;
  }

  out << serialize_instance_toml(tbl);
  out.close();

  std::error_code ec;
  fs::rename(tmp_path, path_, ec);
  if (ec) {
    Logger::instance().error("InstanceRegistry: rename failed: " +
                             ec.message());
    fs::remove(tmp_path, ec); // cleanup
    return false;
  }

  return true;
}

// --- Query ---

std::vector<InstanceRegistry::Entry> InstanceRegistry::all_entries() const {
  return entries_;
}

std::vector<InstanceRegistry::ValidatedEntry>
InstanceRegistry::all_validated() const {
  std::vector<ValidatedEntry> result;
  result.reserve(entries_.size());
  for (const auto &e : entries_) {
    result.push_back({e, validate(e)});
  }
  return result;
}

std::optional<InstanceRegistry::Entry>
InstanceRegistry::find_by_name(const std::string &name) const {
  for (const auto &e : entries_) {
    if (e.name == name)
      return e;
  }
  return std::nullopt;
}

std::optional<InstanceRegistry::Entry>
InstanceRegistry::find_by_root(const fs::path &root) const {
  for (const auto &e : entries_) {
    if (e.root == root.string())
      return e;
  }
  return std::nullopt;
}

std::string InstanceRegistry::active_name() const {
  for (const auto &e : entries_) {
    if (e.is_active)
      return e.name;
  }
  return {};
}

// --- Mutations ---

void InstanceRegistry::register_instance(const std::string &name,
                                         const fs::path &root,
                                         const std::string &type,
                                         const std::string &game_id,
                                         const std::string &display_name) {
  // Check if already registered — update if so
  for (auto &e : entries_) {
    if (e.name == name) {
      e.root = root.string();
      e.type = type;
      e.game_id = game_id;
      e.display_name = display_name.empty() ? name : display_name;
      e.last_used_at = now_iso8601();
      (void)save();
      return;
    }
  }

  // New entry
  Entry entry;
  entry.name = name;
  entry.root = root.string();
  entry.type = type;
  entry.game_id = game_id;
  entry.display_name = display_name.empty() ? name : display_name;
  entry.created_at = now_iso8601();
  entry.last_used_at = entry.created_at;
  entry.is_active = false;

  entries_.push_back(std::move(entry));
  (void)save();
}

void InstanceRegistry::unregister_instance(const std::string &name) {
  auto it = std::remove_if(entries_.begin(), entries_.end(),
                           [&name](const Entry &e) { return e.name == name; });
  if (it != entries_.end()) {
    entries_.erase(it, entries_.end());
    (void)save();
  }
}

void InstanceRegistry::set_active(const std::string &name) {
  for (auto &e : entries_) {
    e.is_active = (e.name == name);
  }
  (void)save();
}

void InstanceRegistry::update_last_used(const std::string &name) {
  for (auto &e : entries_) {
    if (e.name == name) {
      e.last_used_at = now_iso8601();
      (void)save();
      return;
    }
  }
}

void InstanceRegistry::update_root(const std::string &name,
                                   const fs::path &new_root) {
  for (auto &e : entries_) {
    if (e.name == name) {
      e.root = new_root.string();
      (void)save();
      return;
    }
  }
}

// --- Validation ---

std::vector<InstanceRegistry::ValidatedEntry>
InstanceRegistry::validate_all() const {
  std::vector<ValidatedEntry> problems;
  for (const auto &e : entries_) {
    auto status = validate(e);
    if (status != ValidationStatus::Valid) {
      problems.push_back({e, status});
    }
  }
  return problems;
}

InstanceRegistry::ValidationStatus
InstanceRegistry::validate(const Entry &entry) const {
  std::error_code ec;

  // 1. Check if root directory exists
  if (!fs::is_directory(entry.root, ec))
    return ValidationStatus::MissingRoot;

  // 2. Check if instance.toml exists in root
  auto toml_path = fs::path(entry.root) / "instance.toml";
  if (!fs::exists(toml_path, ec))
    return ValidationStatus::MissingToml;

  // 3. Try to parse instance.toml
  auto tbl = parse_instance_toml(toml_path);
  if (!tbl)
    return ValidationStatus::CorruptedToml;

  // 4. Check if game_id is recognized by GameFeatureRegistry
  // Note: This check may return UnregisteredGame for new games not yet
  // registered by plugins. This is a soft warning, not a hard error.
  if (!entry.game_id.empty()) {
    auto &registry = GameFeatureRegistry::instance();
    auto features = registry.features_for(entry.game_id, "mod_data_checker");
    if (features.empty()) {
      // Also check for any feature type for this game
      auto any_features = registry.features_for(entry.game_id, "");
      if (any_features.empty())
        return ValidationStatus::UnregisteredGame;
    }
  }

  return ValidationStatus::Valid;
}

InstanceRegistry::RepairResult
InstanceRegistry::repair_missing(const std::string &name) {
  // This method is called by the UI when validation finds MissingRoot.
  // The UI handles the dialog — this just provides the mutation methods.
  // For now, we just check if the entry exists and return a result.
  auto entry = find_by_name(name);
  if (!entry)
    return {};

  RepairResult result;
  // The actual repair logic will be handled by the UI layer.
  // This method exists as the registry-side API for future UI integration.
  return result;
}

// --- Helpers ---

std::string InstanceRegistry::now_iso8601() const {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif

  std::ostringstream ss;
  ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string InstanceRegistry::sanitize_name(const std::string &name) const {
  return sanitize_directory_name(name);
}

} // namespace engine
