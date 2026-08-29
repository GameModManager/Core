#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/log/logger.h"
#include "engine/deploy/launch/usvfs_library.h"

namespace engine {

struct UsvfsMapping {
  std::filesystem::path source;
  std::filesystem::path destination;
  bool is_directory = true;
  bool create_target = false;
};

class UsvfsConnectorException : public std::runtime_error {
public:
  explicit UsvfsConnectorException(const std::string &msg)
      : std::runtime_error(msg) {}
};

class UsvfsConnector {
public:
  struct Config {
    std::string instance_name = "gamemodmanager_instance";
    ::LogLevel log_level = ::LogLevel::Debug;
    ::CrashDumpsType crash_dump_type = ::CrashDumpsType::Mini;
    std::filesystem::path crash_dump_path;
    std::chrono::milliseconds process_delay{1000};
    std::vector<std::wstring> executable_blacklist;
    std::vector<std::wstring> skip_file_suffixes;
    std::vector<std::wstring> skip_directories;
    std::vector<std::pair<std::wstring, std::wstring>> forced_libraries;
  };

  explicit UsvfsConnector(const Config &config = Config{});
  ~UsvfsConnector();

  UsvfsConnector(const UsvfsConnector &) = delete;
  UsvfsConnector &operator=(const UsvfsConnector &) = delete;
  UsvfsConnector(UsvfsConnector &&) noexcept = delete;
  UsvfsConnector &operator=(UsvfsConnector &&) noexcept = delete;

  bool is_connected() const;

  void update_mapping(const std::vector<UsvfsMapping> &mappings);
  void updateMapping(const std::vector<UsvfsMapping> &mappings) {
    update_mapping(mappings);
  }

  void update_params(const Config &config);
  void updateParams(const Config &config) { update_params(config); }

  void update_params(::LogLevel log_level, ::CrashDumpsType crash_type,
                     const std::filesystem::path &crash_path,
                     std::chrono::milliseconds delay);

  const std::filesystem::path &log_file_path() const;

  // Helpers mirroring MO2's conversion utilities.
  static ::LogLevel to_usvfs_log_level(engine::LogLevel level);
  static ::CrashDumpsType to_usvfs_crash_type(int type);

private:
#ifdef _WIN32
  void init_vfs(const Config &config);
  void apply_blocklists(const Config &config);
  void start_log_worker();
  void stop_log_worker();
  void log_worker_loop();
  static std::filesystem::path default_crash_dump_path();
  static std::string timestamp_string();
  static std::string log_level_string(::LogLevel level);
  static std::string crash_type_string(::CrashDumpsType type);

  UsvfsLibrary library_;
  bool connected_ = false;
  std::thread log_thread_;
  std::atomic<bool> stop_requested_{false};
  std::filesystem::path log_file_path_;
#else
  std::filesystem::path log_file_path_;
#endif
};

}  // namespace engine
