#include "engine/core/instance/instance_manager.h"

#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/toml_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/util/fs_utils.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace engine {

InstanceManager::InstanceManager(fs::path instances_root)
    : root_(std::move(instances_root)) {}

void InstanceManager::ensure_root() const {
  std::error_code ec;
  if (!fs::is_directory(root_, ec)) {
    fs::create_directories(root_, ec);
  }
}

std::vector<Instance> InstanceManager::list_all() const {
  std::vector<Instance> result;
  std::error_code ec;
  if (!fs::is_directory(root_, ec))
    return result;

  for (const auto &entry : fs::directory_iterator(root_, ec)) {
    if (!entry.is_directory())
      continue;
    auto toml = entry.path() / "instance.toml";
    if (!fs::exists(toml))
      continue;
    Instance inst = Instance::from_root(entry.path());
    if (inst.read_toml()) {
      result.push_back(inst);
    }
  }

  // Sort by display_name
  std::sort(result.begin(), result.end(),
            [](const Instance &a, const Instance &b) {
              return a.info().display_name < b.info().display_name;
            });

  return result;
}

std::optional<Instance>
InstanceManager::find_by_name(const std::string &name) const {
  std::error_code ec;
  auto path = root_ / name;
  if (!fs::is_directory(path, ec))
    return std::nullopt;

  auto toml = path / "instance.toml";
  if (!fs::exists(toml))
    return std::nullopt;

  Instance inst = Instance::from_root(path);
  if (!inst.read_toml())
    return std::nullopt;

  return inst;
}

std::string InstanceManager::last_active_name() const {
  return read_last_instance();
}

InstanceManager::CreateResult
InstanceManager::create(const DetectedGame &game,
                        const std::string &display_name) {
  ensure_root();

  Instance inst = create_instance_for_game(game, root_, display_name);
  if (inst.info().game_id.empty()) {
    return {false, "Failed to create instance", {}};
  }

  Logger::instance().info("Instance created: " + inst.info().display_name +
                          " (game=" + game.game_id + ")");
  return {true, "", inst};
}

InstanceManager::CreateResult
InstanceManager::create_portable(const DetectedGame &game,
                                 const std::string &display_name,
                                 const fs::path &root) {
  if (root.empty()) {
    return {false, "Empty root path for portable instance", {}};
  }

  Instance inst = Instance::portable(root);
  inst.info().game_id = game.game_id;
  inst.info().display_name = display_name;
  inst.info().game_dir = game.install_path;
  inst.info().steam_appid = game.steam_appid;

  if (!inst.create_directories()) {
    Logger::instance().error("Failed to create portable instance directories");
    return {false, "Failed to create directories", {}};
  }
  if (!inst.write_toml()) {
    Logger::instance().error("Failed to write instance.toml");
    return {false, "Failed to write instance.toml", {}};
  }

  Logger::instance().info("Portable instance created: " + display_name);
  return {true, "", inst};
}

InstanceManager::RenameResult
InstanceManager::rename(const std::string &current_name,
                        const std::string &new_display_name) {
  auto opt = find_by_name(current_name);
  if (!opt) {
    return {false, "Instance not found: " + current_name, {}};
  }

  Instance inst = *opt;

  // Sanitize new name
  std::string sanitized = Instance::to_instance_name(new_display_name);
  if (sanitized.empty()) {
    return {false, "Invalid instance name", {}};
  }

  // Ensure uniqueness (skip the current name)
  std::string unique_name = sanitized;
  int counter = 2;
  while (true) {
    auto existing = find_by_name(unique_name);
    if (!existing || unique_name == current_name)
      break;
    unique_name = sanitized + " " + std::to_string(counter);
    ++counter;
  }

  // Update display_name in memory
  inst.info().display_name = new_display_name;

  // Write updated TOML
  if (!inst.write_toml()) {
    return {false, "Failed to write instance.toml", {}};
  }

  // Rename folder on disk
  if (unique_name != current_name) {
    fs::path old_path = root_ / current_name;
    fs::path new_path = root_ / unique_name;
    std::error_code ec;
    fs::rename(old_path, new_path, ec);
    if (ec) {
      return {false, "Failed to rename folder: " + ec.message(), {}};
    }

    // If this was the active instance, update last_instance file
    if (is_active(current_name)) {
      write_last_instance(unique_name);
    }
  }

  Logger::instance().info("Instance renamed: " + current_name + " -> " +
                          unique_name);
  return {true, "", unique_name};
}

InstanceManager::DeleteResult InstanceManager::remove(const std::string &name,
                                                      bool force) {
  auto opt = find_by_name(name);
  if (!opt) {
    return {false, "Instance not found: " + name, false};
  }

  bool was_active_flag = is_active(name);

  // Refuse to delete active instance unless forced
  if (was_active_flag && !force) {
    return {false, "Cannot delete active instance. Use force=true to override.",
            true};
  }

  // Remove the directory
  fs::path inst_path = root_ / name;
  std::error_code ec;
  fs::remove_all(inst_path, ec);
  if (ec) {
    return {false, "Failed to remove instance: " + ec.message(),
            was_active_flag};
  }

  // Clear last_instance file if this was the active instance
  if (was_active_flag) {
    write_last_instance("");
  }

  Logger::instance().info("Instance removed: " + name);
  return {true, "", was_active_flag};
}

InstanceManager::CloneResult
InstanceManager::clone(const std::string &source_name,
                       const std::string &new_display_name, bool copy_mods,
                       bool copy_profiles, bool copy_downloads) {
  auto source_opt = find_by_name(source_name);
  if (!source_opt) {
    return {false, "Source instance not found: " + source_name, {}};
  }

  const Instance &source = *source_opt;

  // Create new instance with the new display_name
  DetectedGame game;
  game.game_id = source.info().game_id;
  game.name = new_display_name;
  game.steam_appid = source.info().steam_appid;
  game.install_path = source.info().game_dir;

  auto create_result = create(game, new_display_name);
  if (!create_result.success) {
    return {false, create_result.error, {}};
  }

  Instance &dest = create_result.instance;
  fs::path dest_root = dest.info().root;
  fs::path source_root = source.info().root;

  std::error_code ec;

  // Copy instance.toml (already created by create(), but re-copy to get source
  // fields)
  {
    auto source_tbl = parse_instance_toml(source.toml_path());
    if (source_tbl) {
      // Update the name field
      source_tbl->insert_or_assign("name", new_display_name);
      std::ofstream out(dest.toml_path());
      if (out) {
        out << serialize_instance_toml(*source_tbl);
      }
    }
  }

  // Copy mods directory
  if (copy_mods) {
    fs::path src_mods = source.path_for(InstanceKind::Mods);
    fs::path dst_mods = dest.path_for(InstanceKind::Mods);
    if (fs::is_directory(src_mods, ec)) {
      fs::copy(src_mods, dst_mods,
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing,
               ec);
      if (ec) {
        Logger::instance().warn("Failed to copy mods: " + ec.message());
      }
    }
  }

  // Copy profiles directory
  if (copy_profiles) {
    fs::path src_profiles = source.path_for(InstanceKind::Profiles);
    fs::path dst_profiles = dest.path_for(InstanceKind::Profiles);
    if (fs::is_directory(src_profiles, ec)) {
      fs::copy(src_profiles, dst_profiles,
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing,
               ec);
      if (ec) {
        Logger::instance().warn("Failed to copy profiles: " + ec.message());
      }
    }
  }

  // Copy downloads directory
  if (copy_downloads) {
    fs::path src_downloads = source.path_for(InstanceKind::Downloads);
    fs::path dst_downloads = dest.path_for(InstanceKind::Downloads);
    if (fs::is_directory(src_downloads, ec)) {
      fs::copy(src_downloads, dst_downloads,
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing,
               ec);
      if (ec) {
        Logger::instance().warn("Failed to copy downloads: " + ec.message());
      }
    }
  }

  // Always copy instance.toml (already done above)

  // Never copy: cache/, overwrite/, .gmm_deploy_ledger, .gmm_staging/

  Logger::instance().info("Instance cloned: " + source_name + " -> " +
                          dest.info().display_name);
  return {true, "", dest};
}

InstanceManager::ImportResult
InstanceManager::import_from_path(const fs::path &external_root,
                                  const std::string &display_name) {
  if (!fs::is_directory(external_root)) {
    return {
        false, "Path is not a directory: " + external_root.string(), {}, 0, 0};
  }

  // Check for instance.toml (already an instance?)
  auto toml = external_root / "instance.toml";
  if (fs::exists(toml)) {
    return {false, "Directory already contains an instance.toml", {}, 0, 0};
  }

  // Use the directory name as display_name if not provided
  std::string name =
      display_name.empty() ? external_root.filename().string() : display_name;

  ensure_root();

  // Create a new instance directory and copy content
  std::string sanitized = Instance::to_instance_name(name);
  if (sanitized.empty()) {
    sanitized = "Imported Instance";
  }
  std::string unique_name = unique_instance_name(sanitized, root_);

  Instance inst = Instance::installed(unique_name, root_);
  inst.info().display_name = name;
  inst.info().game_id = ""; // Unknown game

  if (!inst.create_directories()) {
    return {false, "Failed to create instance directories", {}, 0, 0};
  }

  // Copy mods directory
  int mods_imported = 0;
  fs::path src_mods = external_root / "mods";
  if (fs::is_directory(src_mods)) {
    fs::path dst_mods = inst.path_for(InstanceKind::Mods);
    std::error_code ec;
    fs::copy(src_mods, dst_mods,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (!ec) {
      // Count mod folders
      for (const auto &entry : fs::directory_iterator(dst_mods, ec)) {
        if (entry.is_directory())
          ++mods_imported;
      }
    }
  }

  // Copy profiles directory
  int profiles_imported = 0;
  fs::path src_profiles = external_root / "profiles";
  if (fs::is_directory(src_profiles)) {
    fs::path dst_profiles = inst.path_for(InstanceKind::Profiles);
    std::error_code ec;
    fs::copy(src_profiles, dst_profiles,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
    if (!ec) {
      for (const auto &entry : fs::directory_iterator(dst_profiles, ec)) {
        if (entry.is_directory())
          ++profiles_imported;
      }
    }
  }

  // Copy downloads directory
  fs::path src_downloads = external_root / "downloads";
  if (fs::is_directory(src_downloads)) {
    fs::path dst_downloads = inst.path_for(InstanceKind::Downloads);
    std::error_code ec;
    fs::copy(src_downloads, dst_downloads,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing,
             ec);
  }

  // Write instance.toml
  if (!inst.write_toml()) {
    return {false, "Failed to write instance.toml", {}, 0, 0};
  }

  Logger::instance().info("Instance imported from path: " +
                          external_root.string());
  return {true, "", inst, mods_imported, profiles_imported};
}

fs::path InstanceManager::instances_root() const { return root_; }

bool InstanceManager::is_active(const std::string &name) const {
  return read_last_instance() == name;
}

} // namespace engine
