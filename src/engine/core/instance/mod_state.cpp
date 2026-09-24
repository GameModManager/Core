#include "engine/core/instance/mod_state.h"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

namespace engine {

ModStateTracker::ModStateTracker(std::filesystem::path instance_root)
    : instance_root_(std::move(instance_root)) {}

bool ModStateTracker::load() {
  auto path = instance_root_ / "mod_state.json";
  std::ifstream in(path);
  if (!in)
    return true;  // missing file = empty state

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(in);
  } catch (const nlohmann::json::parse_error &) {
    return false;
  }

  entries_.clear();
  if (auto it = j.find("entries"); it != j.end() && it->is_object()) {
    for (auto &[key, val] : it->items()) {
      if (!val.is_object())
        continue;
      ModTrackingEntry e;
      e.install_order    = val.value("install_order", 0u);
      e.installed_at     = val.value("installed_at", int64_t(0));
      e.list_position    = val.value("list_position", -1);
      e.parent_separator = val.value("parent_separator", std::string());
      e.depth            = val.value("depth", 0);
      e.separator_color  = val.value("separator_color", std::string());
      e.collapsed        = val.value("collapsed", false);
      e.hidden           = val.value("hidden", false);
      e.disabled         = val.value("disabled", false);
      e.deploy_at_root   = val.value("deploy_at_root", false);
      if (auto hf = val.find("hidden_files"); hf != val.end() && hf->is_array()) {
        e.hidden_files = hf->get<std::vector<std::string>>();
      }
      entries_[key] = std::move(e);
    }
  }

  return true;
}

bool ModStateTracker::save() const {
  nlohmann::json j;
  j["version"] = 1;

  nlohmann::json entries_obj;
  for (const auto &[key, e] : entries_) {
    nlohmann::json val;
    val["install_order"] = e.install_order;
    val["installed_at"]  = e.installed_at;
    val["list_position"] = e.list_position;
    if (!e.parent_separator.empty())
      val["parent_separator"] = e.parent_separator;
    val["depth"] = e.depth;
    if (!e.separator_color.empty())
      val["separator_color"] = e.separator_color;
    if (e.collapsed)
      val["collapsed"] = true;
    if (e.hidden)
      val["hidden"] = true;
    if (e.disabled)
      val["disabled"] = true;
    if (!e.hidden_files.empty())
      val["hidden_files"] = e.hidden_files;
    if (e.deploy_at_root)
      val["deploy_at_root"] = true;
    entries_obj[key] = std::move(val);
  }
  j["entries"] = std::move(entries_obj);

  // Atomic write: write to temp, then rename.
  auto path = instance_root_ / "mod_state.json";
  auto tmp  = path;
  tmp += ".tmp";

  std::ofstream out(tmp);
  if (!out)
    return false;
  out << j.dump(2) << "\n";
  if (!out.good()) {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return false;
  }
  out.close();

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

ModTrackingEntry &ModStateTracker::entry(const std::string &folder_name) {
  return entries_[folder_name];
}

const ModTrackingEntry *ModStateTracker::entry(const std::string &folder_name) const {
  auto it = entries_.find(folder_name);
  return it != entries_.end() ? &it->second : nullptr;
}

void ModStateTracker::remove(const std::string &folder_name) {
  entries_.erase(folder_name);
}

void ModStateTracker::record_install(const std::string &folder_name) {
  auto &e         = entries_[folder_name];
  e.install_order = next_install_order();
  e.installed_at  = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
}

void ModStateTracker::set_position(const std::string &folder_name, int32_t position) {
  entries_[folder_name].list_position = position;
}

void ModStateTracker::set_nesting(const std::string &folder_name,
                                  const std::string &parent_separator, int32_t depth) {
  auto &e            = entries_[folder_name];
  e.parent_separator = parent_separator;
  e.depth            = depth;
}

void ModStateTracker::set_separator_visual(const std::string &folder_name,
                                           const std::string &color, bool collapsed) {
  auto &e           = entries_[folder_name];
  e.separator_color = color;
  e.collapsed       = collapsed;
}

void ModStateTracker::set_hidden(const std::string &folder_name, bool hidden) {
  entries_[folder_name].hidden = hidden;
}

void ModStateTracker::set_disabled(const std::string &folder_name, bool disabled) {
  entries_[folder_name].disabled = disabled;
}

void ModStateTracker::set_hidden_files(const std::string &folder_name,
                                       std::vector<std::string> files) {
  entries_[folder_name].hidden_files = std::move(files);
}

void ModStateTracker::set_deploy_at_root(const std::string &folder_name, bool at_root) {
  entries_[folder_name].deploy_at_root = at_root;
}

uint32_t ModStateTracker::next_install_order() const {
  uint32_t max_order = 0;
  for (const auto &[_, e] : entries_) {
    if (e.install_order > max_order)
      max_order = e.install_order;
  }
  return max_order + 1;
}

const std::unordered_map<std::string, ModTrackingEntry> &
ModStateTracker::all_entries() const {
  return entries_;
}

void ModStateTracker::set_all_entries(
    std::unordered_map<std::string, ModTrackingEntry> entries) {
  entries_ = std::move(entries);
}

const std::filesystem::path &ModStateTracker::instance_root() const {
  return instance_root_;
}

}  // namespace engine
