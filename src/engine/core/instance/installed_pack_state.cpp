#include "engine/core/instance/installed_pack_state.h"

#include "engine/profile/safe_write_file.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace engine {

namespace {

constexpr int kStateVersion = 1;

}  // namespace

InstalledPackState::InstalledPackState(std::filesystem::path instance_root)
    : instance_root_(std::move(instance_root)) {}

std::filesystem::path InstalledPackState::file_path() const {
  return instance_root_ / "installed_pack.json";
}

bool InstalledPackState::load() {
  std::ifstream in(file_path());
  if (!in) {
    return true;  // missing file = empty state
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(in);
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  if (!j.is_object()) {
    return false;
  }

  // Parse into locals first: a corrupt file leaves in-memory state untouched.
  try {
    std::string pack_id;
    int64_t revision = 0;
    std::unordered_map<std::string, ResolvedModEntry> mods;
    std::string tree_snapshot;
    std::vector<std::string> patches;
    std::vector<std::string> ini_edits;
    std::unordered_map<std::string, bool> tweaks;

    if (auto it = j.find("packId"); it != j.end()) {
      pack_id = it->get<std::string>();
    }
    if (auto it = j.find("installedRevision"); it != j.end()) {
      revision = it->get<int64_t>();
    }
    if (auto it = j.find("resolvedMods"); it != j.end() && it->is_object()) {
      for (auto& [key, val] : it->items()) {
        if (!val.is_object()) {
          continue;
        }
        ResolvedModEntry e;
        e.origin = pack_origin_from_string(val.value("origin", std::string()));
        e.presence = pack_presence_from_string(val.value("presence", std::string()));
        e.placement = pack_placement_from_string(val.value("placement", std::string()));
        e.last_applied_revision = val.value("lastAppliedRevision", int64_t(0));
        e.actual_file_id = val.value("actualFileId", int64_t(0));
        e.actual_version = val.value("actualVersion", std::string());
        e.actual_hash = val.value("actualHash", std::string());
        e.produced_by = val.value("producedBy", std::string());
        mods[key] = std::move(e);
      }
    }
    if (auto it = j.find("treeSnapshot"); it != j.end()) {
      tree_snapshot = it->get<std::string>();
    }
    if (auto it = j.find("appliedPatches"); it != j.end() && it->is_array()) {
      patches = it->get<std::vector<std::string>>();
    }
    if (auto it = j.find("appliedIniEdits"); it != j.end() && it->is_array()) {
      ini_edits = it->get<std::vector<std::string>>();
    }
    if (auto it = j.find("iniTweaks"); it != j.end() && it->is_object()) {
      for (auto& [key, val] : it->items()) {
        tweaks[key] = val.get<bool>();
      }
    }

    pack_id_ = std::move(pack_id);
    installed_revision_ = revision;
    resolved_mods_ = std::move(mods);
    tree_snapshot_ = std::move(tree_snapshot);
    applied_patches_ = std::move(patches);
    applied_ini_edits_ = std::move(ini_edits);
    ini_tweaks_ = std::move(tweaks);
  } catch (const nlohmann::json::exception&) {
    return false;
  }
  return true;
}

bool InstalledPackState::save() const {
  nlohmann::json j;
  j["version"] = kStateVersion;
  j["packId"] = pack_id_;
  j["installedRevision"] = installed_revision_;

  nlohmann::json mods_obj = nlohmann::json::object();
  for (const auto& [key, e] : resolved_mods_) {
    nlohmann::json val;
    val["origin"] = to_string(e.origin);
    val["presence"] = to_string(e.presence);
    val["placement"] = to_string(e.placement);
    val["lastAppliedRevision"] = e.last_applied_revision;
    val["actualFileId"] = e.actual_file_id;
    if (!e.actual_version.empty()) {
      val["actualVersion"] = e.actual_version;
    }
    if (!e.actual_hash.empty()) {
      val["actualHash"] = e.actual_hash;
    }
    if (!e.produced_by.empty()) {
      val["producedBy"] = e.produced_by;
    }
    mods_obj[key] = std::move(val);
  }
  j["resolvedMods"] = std::move(mods_obj);
  j["treeSnapshot"] = tree_snapshot_;
  j["appliedPatches"] = applied_patches_;
  j["appliedIniEdits"] = applied_ini_edits_;

  nlohmann::json tweaks_obj = nlohmann::json::object();
  for (const auto& [key, enabled] : ini_tweaks_) {
    tweaks_obj[key] = enabled;
  }
  j["iniTweaks"] = std::move(tweaks_obj);

  return profile::safe_write_file(file_path(), j.dump(2) + "\n");
}

void InstalledPackState::clear() {
  pack_id_.clear();
  installed_revision_ = 0;
  resolved_mods_.clear();
  tree_snapshot_.clear();
  applied_patches_.clear();
  applied_ini_edits_.clear();
  ini_tweaks_.clear();
}

bool InstalledPackState::has_pack() const {
  return !pack_id_.empty();
}

void InstalledPackState::set_pack(std::string pack_id, int64_t revision) {
  pack_id_ = std::move(pack_id);
  installed_revision_ = revision;
}

const std::string& InstalledPackState::pack_id() const {
  return pack_id_;
}

int64_t InstalledPackState::installed_revision() const {
  return installed_revision_;
}

ResolvedModEntry& InstalledPackState::ensure_pack_mod(const std::string& mod_id) {
  return resolved_mods_[mod_id];
}

void InstalledPackState::record_pack_version(const std::string& mod_id,
                                             int64_t file_id,
                                             const std::string& version,
                                             const std::string& hash,
                                             int64_t revision) {
  auto& e = resolved_mods_[mod_id];
  e.actual_file_id = file_id;
  e.actual_version = version;
  e.actual_hash = hash;
  e.last_applied_revision = revision;
}

void InstalledPackState::mark_manual(const std::string& mod_id) {
  resolved_mods_[mod_id].origin = PackModOrigin::Manual;
}

void InstalledPackState::mark_generated(const std::string& mod_id,
                                        const std::string& produced_by) {
  auto& e = resolved_mods_[mod_id];
  e.origin = PackModOrigin::Generated;
  e.produced_by = produced_by;
}

void InstalledPackState::mark_removed(const std::string& mod_id) {
  resolved_mods_[mod_id].presence = PackModPresence::Removed;
}

void InstalledPackState::restore_from_pack(const std::string& mod_id) {
  resolved_mods_[mod_id].presence = PackModPresence::Installed;
}

void InstalledPackState::mark_diverged(const std::string& mod_id) {
  resolved_mods_[mod_id].placement = PackModPlacement::Diverged;
}

void InstalledPackState::reset_to_pack_layout(const std::string& mod_id) {
  resolved_mods_[mod_id].placement = PackModPlacement::Conforming;
}

const ResolvedModEntry* InstalledPackState::resolved_mod(
    const std::string& mod_id) const {
  auto it = resolved_mods_.find(mod_id);
  return it != resolved_mods_.end() ? &it->second : nullptr;
}

bool InstalledPackState::is_tracked(const std::string& mod_id) const {
  return resolved_mods_.find(mod_id) != resolved_mods_.end();
}

bool InstalledPackState::skip_in_update(const std::string& mod_id) const {
  auto it = resolved_mods_.find(mod_id);
  if (it == resolved_mods_.end()) {
    return false;  // untracked = new mod, fresh-install it
  }
  return it->second.origin == PackModOrigin::Manual ||
         it->second.presence == PackModPresence::Removed;
}

bool InstalledPackState::follows_pack_tree(const std::string& mod_id) const {
  auto it = resolved_mods_.find(mod_id);
  if (it == resolved_mods_.end()) {
    return false;
  }
  const auto& e = it->second;
  return e.origin != PackModOrigin::Manual &&
         e.presence == PackModPresence::Installed &&
         e.placement == PackModPlacement::Conforming;
}

const std::unordered_map<std::string, ResolvedModEntry>&
InstalledPackState::all_resolved_mods() const {
  return resolved_mods_;
}

void InstalledPackState::set_tree_snapshot(std::string snapshot) {
  tree_snapshot_ = std::move(snapshot);
}

const std::string& InstalledPackState::tree_snapshot() const {
  return tree_snapshot_;
}

void InstalledPackState::set_applied_patches(std::vector<std::string> patches) {
  applied_patches_ = std::move(patches);
}

const std::vector<std::string>& InstalledPackState::applied_patches() const {
  return applied_patches_;
}

void InstalledPackState::set_applied_ini_edits(std::vector<std::string> edits) {
  applied_ini_edits_ = std::move(edits);
}

const std::vector<std::string>& InstalledPackState::applied_ini_edits() const {
  return applied_ini_edits_;
}

void InstalledPackState::set_ini_tweak(const std::string& tweak_id, bool enabled) {
  ini_tweaks_[tweak_id] = enabled;
}

std::optional<bool> InstalledPackState::ini_tweak_enabled(
    const std::string& tweak_id) const {
  auto it = ini_tweaks_.find(tweak_id);
  return it != ini_tweaks_.end() ? std::optional<bool>(it->second) : std::nullopt;
}

const std::unordered_map<std::string, bool>& InstalledPackState::ini_tweaks() const {
  return ini_tweaks_;
}

const std::filesystem::path& InstalledPackState::instance_root() const {
  return instance_root_;
}

const char* to_string(PackModOrigin origin) {
  switch (origin) {
    case PackModOrigin::Pack:
      return "pack";
    case PackModOrigin::Manual:
      return "manual";
    case PackModOrigin::Generated:
      return "generated";
  }
  return "pack";
}

const char* to_string(PackModPresence presence) {
  switch (presence) {
    case PackModPresence::Installed:
      return "installed";
    case PackModPresence::Removed:
      return "removed";
  }
  return "installed";
}

const char* to_string(PackModPlacement placement) {
  switch (placement) {
    case PackModPlacement::Conforming:
      return "conforming";
    case PackModPlacement::Diverged:
      return "diverged";
  }
  return "conforming";
}

PackModOrigin pack_origin_from_string(const std::string& s) {
  if (s == "manual") {
    return PackModOrigin::Manual;
  }
  if (s == "generated") {
    return PackModOrigin::Generated;
  }
  return PackModOrigin::Pack;
}

PackModPresence pack_presence_from_string(const std::string& s) {
  if (s == "removed") {
    return PackModPresence::Removed;
  }
  return PackModPresence::Installed;
}

PackModPlacement pack_placement_from_string(const std::string& s) {
  if (s == "diverged") {
    return PackModPlacement::Diverged;
  }
  return PackModPlacement::Conforming;
}

}  // namespace engine
