#pragma once

#include "engine/core/instance/instance.h"
#include "engine/game/detect/game_detector.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace engine {

// Centralizes ALL instance lifecycle operations: list, create, rename, delete,
// clone, import from MO2. Wraps the existing free functions in instance_utils
// and Instance methods into a cohesive manager object.
class InstanceManager {
public:
  // Construction
  explicit InstanceManager(std::filesystem::path instances_root);

  // Discovery
  std::vector<Instance> list_all() const;
  std::optional<Instance> find_by_name(const std::string &name) const;
  std::string last_active_name() const;

  // Create
  struct CreateResult {
    bool success = false;
    std::string error;
    Instance instance;
  };
  CreateResult create(const DetectedGame &game,
                      const std::string &display_name);
  CreateResult create_portable(const DetectedGame &game,
                               const std::string &display_name,
                               const std::filesystem::path &root);

  // Rename
  struct RenameResult {
    bool success = false;
    std::string error;
    std::string new_name; // sanitized folder name
  };
  RenameResult rename(const std::string &current_name,
                      const std::string &new_display_name);

  // Delete
  struct DeleteResult {
    bool success = false;
    std::string error;
    bool was_active = false;
  };
  DeleteResult remove(const std::string &name, bool force = false);

  // Clone
  struct CloneResult {
    bool success = false;
    std::string error;
    Instance instance;
  };
  CloneResult clone(const std::string &source_name,
                    const std::string &new_display_name, bool copy_mods = true,
                    bool copy_profiles = true, bool copy_downloads = false);

  // Import from MO2
  struct ImportResult {
    bool success = false;
    std::string error;
    Instance instance;
    int mods_imported = 0;
    int profiles_imported = 0;
  };
  ImportResult import_from_mo2(const std::filesystem::path &mo2_instance_dir,
                               const std::string &display_name = "");

  // Import from path (generic — wraps an existing directory as an instance)
  ImportResult import_from_path(const std::filesystem::path &external_root,
                                const std::string &display_name);

  // Utility
  std::filesystem::path instances_root() const;
  bool is_active(const std::string &name) const;

private:
  std::filesystem::path root_;
  void ensure_root() const;
};

} // namespace engine
