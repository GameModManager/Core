#include "engine/deploy/launch/usvfs_connector.h"

#include "engine/core/log/logger.h"

#ifdef _WIN32

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace engine {

namespace {

constexpr unsigned int kLinkFlagCreateTarget = 0x00000004;
constexpr unsigned int kLinkFlagRecursive = 0x00000008;

std::string to_string(::LogLevel level) {
  switch (level) {
    case ::LogLevel::Debug:
      return "debug";
    case ::LogLevel::Info:
      return "info";
    case ::LogLevel::Warning:
      return "warning";
    case ::LogLevel::Error:
      return "error";
    default:
      return "debug";
  }
}

std::string crash_to_string(::CrashDumpsType type) {
  switch (type) {
    case ::CrashDumpsType::None:
      return "none";
    case ::CrashDumpsType::Mini:
      return "mini";
    case ::CrashDumpsType::Data:
      return "data";
    case ::CrashDumpsType::Full:
      return "full";
    default:
      return "mini";
  }
}

}  // namespace

UsvfsConnector::UsvfsConnector(const Config &config) {
  init_vfs(config);
  if (connected_) {
    start_log_worker();
  }
}

UsvfsConnector::~UsvfsConnector() {
  if (connected_ && library_.loaded() && library_.disconnect_vfs()) {
    library_.disconnect_vfs()();
  }
  connected_ = false;
  stop_log_worker();
}

bool UsvfsConnector::is_connected() const { return connected_; }

const std::filesystem::path &UsvfsConnector::log_file_path() const {
  return log_file_path_;
}

void UsvfsConnector::update_mapping(const std::vector<UsvfsMapping> &mappings) {
  if (!library_.loaded() || !connected_) {
    Logger::instance().warn(
        "UsvfsConnector::update_mapping called while VFS not connected");
    return;
  }
  auto *clear_fn = library_.clear_virtual_mappings();
  auto *link_dir_fn = library_.virtual_link_directory_static();
  auto *link_file_fn = library_.virtual_link_file();
  if (!clear_fn || !link_dir_fn || !link_file_fn) {
    Logger::instance().error(
        "UsvfsConnector::update_mapping missing USVFS symbols");
    return;
  }

  Logger::instance().debug("UsvfsConnector: updating VFS mappings (" +
                           std::to_string(mappings.size()) + " entries)");
  clear_fn();

  int dirs = 0;
  int files = 0;
  auto start = std::chrono::steady_clock::now();

  for (const auto &m : mappings) {
    if (m.is_directory) {
      unsigned int flags = kLinkFlagRecursive;
      if (m.create_target) {
        flags |= kLinkFlagCreateTarget;
      }
      std::wstring src = m.source.wstring();
      std::wstring dst = m.destination.wstring();
      BOOL ok = link_dir_fn(src.c_str(), dst.c_str(), flags);
      if (!ok) {
        Logger::instance().warn("UsvfsConnector: VirtualLinkDirectoryStatic failed: " +
                                m.source.string() + " -> " + m.destination.string());
      } else {
        ++dirs;
      }
    } else {
      std::wstring src = m.source.wstring();
      std::wstring dst = m.destination.wstring();
      BOOL ok = link_file_fn(src.c_str(), dst.c_str(), 0);
      if (!ok) {
        Logger::instance().warn("UsvfsConnector: VirtualLinkFile failed: " +
                                m.source.string() + " -> " + m.destination.string());
      } else {
        ++files;
      }
    }
  }

  auto end = std::chrono::steady_clock::now();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  Logger::instance().debug("UsvfsConnector: VFS mappings updated dirs=" +
                           std::to_string(dirs) + " files=" + std::to_string(files) +
                           " in " + std::to_string(ms) + "ms");
}

void UsvfsConnector::update_params(const Config &config) {
  if (!library_.loaded()) {
    Logger::instance().warn(
        "UsvfsConnector::update_params called while library not loaded");
    return;
  }
  auto *create_fn = library_.create_parameters();
  auto *update_fn = library_.update_parameters();
  auto *free_fn = library_.free_parameters();
  if (!create_fn || !update_fn || !free_fn) {
    Logger::instance().error("UsvfsConnector::update_params missing symbols");
    return;
  }

  auto crash_path = config.crash_dump_path.empty() ? default_crash_dump_path()
                                                   : config.crash_dump_path;
  std::error_code ec;
  std::filesystem::create_directories(crash_path, ec);

  usvfsParameters *p = create_fn();
  if (!p) {
    Logger::instance().error("UsvfsConnector: usvfsCreateParameters failed in update_params");
    return;
  }
  if (library_.set_debug_mode()) {
    library_.set_debug_mode()(p, FALSE);
  }
  if (library_.set_log_level()) {
    library_.set_log_level()(p, config.log_level);
  }
  if (library_.set_crash_dump_type()) {
    library_.set_crash_dump_type()(p, config.crash_dump_type);
  }
  if (library_.set_crash_dump_path()) {
    library_.set_crash_dump_path()(p, crash_path.string().c_str());
  }
  if (library_.set_process_delay()) {
    library_.set_process_delay()(p, static_cast<int>(config.process_delay.count()));
  }
  update_fn(p);
  free_fn(p);

  apply_blocklists(config);

  Logger::instance().debug(
      "UsvfsConnector: updated params log=" + log_level_string(config.log_level) +
      " dump=" + crash_path.string() + " delay=" +
      std::to_string(config.process_delay.count()) + "ms");
}

void UsvfsConnector::update_params(::LogLevel log_level, ::CrashDumpsType crash_type,
                                   const std::filesystem::path &crash_path,
                                   std::chrono::milliseconds delay) {
  Config cfg;
  cfg.log_level = log_level;
  cfg.crash_dump_type = crash_type;
  cfg.crash_dump_path = crash_path;
  cfg.process_delay = delay;
  update_params(cfg);
}

::LogLevel UsvfsConnector::to_usvfs_log_level(engine::LogLevel level) {
  switch (level) {
    case engine::LogLevel::Info:
      return ::LogLevel::Info;
    case engine::LogLevel::Warn:
      return ::LogLevel::Warning;
    case engine::LogLevel::Error:
      return ::LogLevel::Error;
    case engine::LogLevel::Debug:
    default:
      return ::LogLevel::Debug;
  }
}

::CrashDumpsType UsvfsConnector::to_usvfs_crash_type(int type) {
  // Map integer (0=None,1=Mini,2=Data,3=Full) to enum. Keep compatible with
  // simple int settings.
  switch (type) {
    case 0:
      return ::CrashDumpsType::None;
    case 2:
      return ::CrashDumpsType::Data;
    case 3:
      return ::CrashDumpsType::Full;
    case 1:
    default:
      return ::CrashDumpsType::Mini;
  }
}

void UsvfsConnector::init_vfs(const Config &config) {
  if (!library_.loaded()) {
    Logger::instance().error(
        "UsvfsConnector: usvfs library not loaded, VFS unavailable");
    return;
  }

  auto *create_fn = library_.create_parameters();
  auto *free_fn = library_.free_parameters();
  auto *init_log_fn = library_.init_logging();
  auto *create_vfs_fn = library_.create_vfs();
  if (!create_fn || !free_fn || !create_vfs_fn) {
    Logger::instance().error("UsvfsConnector: missing USVFS symbols for init");
    return;
  }

  auto crash_path = config.crash_dump_path.empty() ? default_crash_dump_path()
                                                   : config.crash_dump_path;
  std::error_code ec;
  std::filesystem::create_directories(crash_path, ec);
  if (ec) {
    Logger::instance().warn("UsvfsConnector: failed to create crash dump dir " +
                            crash_path.string() + ": " + ec.message());
  }

  usvfsParameters *params = create_fn();
  if (!params) {
    Logger::instance().error("UsvfsConnector: usvfsCreateParameters failed");
    return;
  }

  if (library_.set_instance_name()) {
    library_.set_instance_name()(params, config.instance_name.c_str());
  }
  if (library_.set_debug_mode()) {
    library_.set_debug_mode()(params, FALSE);
  }
  if (library_.set_log_level()) {
    library_.set_log_level()(params, config.log_level);
  }
  if (library_.set_crash_dump_type()) {
    library_.set_crash_dump_type()(params, config.crash_dump_type);
  }
  if (library_.set_crash_dump_path()) {
    library_.set_crash_dump_path()(params, crash_path.string().c_str());
  }
  if (library_.set_process_delay()) {
    library_.set_process_delay()(params,
                                 static_cast<int>(config.process_delay.count()));
  }

  if (init_log_fn) {
    init_log_fn(false);
  }

  std::string level_str = log_level_string(config.log_level);
  std::string crash_str = crash_type_string(config.crash_dump_type);
  if (library_.log_level_to_string() && library_.crash_dump_type_to_string()) {
    level_str = library_.log_level_to_string()(config.log_level);
    crash_str = library_.crash_dump_type_to_string()(config.crash_dump_type);
  }
  Logger::instance().debug("UsvfsConnector: initializing usvfs instance=" +
                           config.instance_name + " log=" + level_str +
                           " dump=" + crash_path.string() + " (" + crash_str + ")" +
                           " delay=" + std::to_string(config.process_delay.count()) + "ms");

  BOOL ok = create_vfs_fn(params);
  free_fn(params);

  if (!ok) {
    Logger::instance().error("UsvfsConnector: usvfsCreateVFS failed");
    return;
  }

  connected_ = true;
  apply_blocklists(config);
  Logger::instance().debug("UsvfsConnector: VFS created");
}

void UsvfsConnector::apply_blocklists(const Config &config) {
  if (library_.clear_executable_blacklist()) {
    library_.clear_executable_blacklist()();
  }
  if (library_.blacklist_executable()) {
    for (const auto &exe : config.executable_blacklist) {
      if (!exe.empty()) {
        library_.blacklist_executable()(exe.c_str());
      }
    }
  }

  if (library_.clear_skip_file_suffixes()) {
    library_.clear_skip_file_suffixes()();
  }
  if (library_.add_skip_file_suffix()) {
    for (const auto &s : config.skip_file_suffixes) {
      if (!s.empty()) {
        library_.add_skip_file_suffix()(s.c_str());
      }
    }
  }

  if (library_.clear_skip_directories()) {
    library_.clear_skip_directories()();
  }
  if (library_.add_skip_directory()) {
    for (const auto &d : config.skip_directories) {
      if (!d.empty()) {
        library_.add_skip_directory()(d.c_str());
      }
    }
  }

  if (library_.clear_library_force_loads()) {
    library_.clear_library_force_loads()();
  }
  if (library_.force_load_library()) {
    for (const auto &p : config.forced_libraries) {
      if (!p.first.empty() && !p.second.empty()) {
        library_.force_load_library()(p.first.c_str(), p.second.c_str());
      }
    }
  }
}

void UsvfsConnector::start_log_worker() {
  if (log_thread_.joinable()) {
    return;
  }
  // Prepare log file path: logs/usvfs-<ts>.log relative to current dir.
  log_file_path_ = std::filesystem::path("logs") /
                   ("usvfs-" + timestamp_string() + ".log");
  std::error_code ec;
  std::filesystem::create_directories(log_file_path_.parent_path(), ec);
  if (ec) {
    Logger::instance().warn("UsvfsConnector: failed to create logs dir: " + ec.message());
  } else {
    // Truncate/create file so it exists even before first message.
    std::ofstream probe(log_file_path_, std::ios::app);
    if (probe) {
      probe.close();
    }
    Logger::instance().debug("UsvfsConnector: usvfs log file " +
                             log_file_path_.string());
  }

  stop_requested_.store(false);
  log_thread_ = std::thread([this] { log_worker_loop(); });
}

void UsvfsConnector::stop_log_worker() {
  stop_requested_.store(true);
  if (log_thread_.joinable()) {
    log_thread_.join();
  }
}

void UsvfsConnector::log_worker_loop() {
  auto *get_log_fn = library_.get_log_messages();
  if (!get_log_fn) {
    return;
  }

  std::vector<char> buffer(1024, '\0');
  std::ofstream out;
  // Open lazily on first message to avoid holding handle if VFS never logs.
  bool file_opened = false;
  auto ensure_open = [&]() {
    if (!file_opened) {
      out.open(log_file_path_, std::ios::app);
      file_opened = out.is_open();
      if (!file_opened) {
        Logger::instance().error("UsvfsConnector: failed to open usvfs log file " +
                                 log_file_path_.string());
      }
    }
  };

  int no_log_cycles = 0;
  while (!stop_requested_.load()) {
    // usvfsGetLogMessages expects a char buffer and size; it returns true
    // when a message was written.
    bool has_msg = false;
    // Reset buffer to zeros to handle short messages.
    std::fill(buffer.begin(), buffer.end(), '\0');
    has_msg = get_log_fn(buffer.data(), buffer.size(), false);
    if (has_msg) {
      ensure_open();
      if (file_opened) {
        // buffer is null-terminated C string; write without extra nulls.
        out << buffer.data() << "\n";
        out.flush();
      }
      no_log_cycles = 0;
    } else {
      int sleep_ms = std::min(40, no_log_cycles) * 5;
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      ++no_log_cycles;
    }
  }
  if (out.is_open()) {
    out.flush();
    out.close();
  }
}

std::filesystem::path UsvfsConnector::default_crash_dump_path() {
  // GMM cache/crashdumps relative to current working dir. The caller may
  // override via Config::crash_dump_path (e.g. from PlatformInterface::cache_dir).
  return std::filesystem::path("cache") / "crashdumps";
}

std::string UsvfsConnector::timestamp_string() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
  return std::string(buf);
}

std::string UsvfsConnector::log_level_string(::LogLevel level) { return to_string(level); }

std::string UsvfsConnector::crash_type_string(::CrashDumpsType type) {
  return crash_to_string(type);
}

}  // namespace engine

#else  // !_WIN32 — stub implementation so header compiles everywhere

namespace engine {

UsvfsConnector::UsvfsConnector(const Config &) {}
UsvfsConnector::~UsvfsConnector() {}
bool UsvfsConnector::is_connected() const { return false; }
const std::filesystem::path &UsvfsConnector::log_file_path() const {
  return log_file_path_;
}
void UsvfsConnector::update_mapping(const std::vector<UsvfsMapping> &) {}
void UsvfsConnector::update_params(const Config &) {}
void UsvfsConnector::update_params(::LogLevel, ::CrashDumpsType,
                                   const std::filesystem::path &,
                                   std::chrono::milliseconds) {}
::LogLevel UsvfsConnector::to_usvfs_log_level(engine::LogLevel level) {
  switch (level) {
    case engine::LogLevel::Info:
      return ::LogLevel::Info;
    case engine::LogLevel::Warn:
      return ::LogLevel::Warning;
    case engine::LogLevel::Error:
      return ::LogLevel::Error;
    case engine::LogLevel::Debug:
    default:
      return ::LogLevel::Debug;
  }
}
::CrashDumpsType UsvfsConnector::to_usvfs_crash_type(int type) {
  switch (type) {
    case 0:
      return ::CrashDumpsType::None;
    case 2:
      return ::CrashDumpsType::Data;
    case 3:
      return ::CrashDumpsType::Full;
    case 1:
    default:
      return ::CrashDumpsType::Mini;
  }
}

}  // namespace engine

#endif  // _WIN32
