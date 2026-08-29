#include "engine/deploy/launch/usvfs_library.h"

#include "engine/core/log/logger.h"

#ifdef _WIN32

#include <string>

namespace engine {

namespace {

std::string format_last_error(DWORD err) {
  LPWSTR buf = nullptr;
  DWORD len = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::string out;
  if (len && buf) {
    int needed = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                                     nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
      out.resize(static_cast<size_t>(needed));
      WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), out.data(),
                          needed, nullptr, nullptr);
      while (!out.empty() &&
             (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
      }
    }
    LocalFree(buf);
  }
  if (out.empty()) {
    out = "error " + std::to_string(err);
  }
  return out;
}

constexpr const char *kAvHint = " This may indicate antivirus quarantine — "
                                "check your AV quarantine for usvfs_x64.dll"
                                " and ensure it sits next to GMM.exe.";

} // namespace

UsvfsLibrary::UsvfsLibrary() { load(); }

UsvfsLibrary::~UsvfsLibrary() {
  if (handle_) {
    FreeLibrary(handle_);
    handle_ = nullptr;
  }
}

UsvfsLibrary::UsvfsLibrary(UsvfsLibrary &&other) noexcept
    : handle_(other.handle_), loaded_(other.loaded_),
      create_parameters_(other.create_parameters_),
      set_instance_name_(other.set_instance_name_),
      set_debug_mode_(other.set_debug_mode_),
      set_log_level_(other.set_log_level_),
      set_crash_dump_type_(other.set_crash_dump_type_),
      set_crash_dump_path_(other.set_crash_dump_path_),
      set_process_delay_(other.set_process_delay_),
      free_parameters_(other.free_parameters_), create_vfs_(other.create_vfs_),
      disconnect_vfs_(other.disconnect_vfs_),
      clear_virtual_mappings_(other.clear_virtual_mappings_),
      virtual_link_directory_static_(other.virtual_link_directory_static_),
      virtual_link_file_(other.virtual_link_file_),
      create_process_hooked_(other.create_process_hooked_),
      blacklist_executable_(other.blacklist_executable_),
      clear_executable_blacklist_(other.clear_executable_blacklist_),
      add_skip_file_suffix_(other.add_skip_file_suffix_),
      clear_skip_file_suffixes_(other.clear_skip_file_suffixes_),
      add_skip_directory_(other.add_skip_directory_),
      clear_skip_directories_(other.clear_skip_directories_),
      force_load_library_(other.force_load_library_),
      clear_library_force_loads_(other.clear_library_force_loads_),
      init_logging_(other.init_logging_),
      get_log_messages_(other.get_log_messages_),
      get_vfs_process_list2_(other.get_vfs_process_list2_),
      update_parameters_(other.update_parameters_),
      create_mini_dump_(other.create_mini_dump_),
      log_level_to_string_(other.log_level_to_string_),
      crash_dump_type_to_string_(other.crash_dump_type_to_string_) {
  other.handle_ = nullptr;
  other.loaded_ = false;
  other.create_parameters_ = nullptr;
  other.set_instance_name_ = nullptr;
  other.set_debug_mode_ = nullptr;
  other.set_log_level_ = nullptr;
  other.set_crash_dump_type_ = nullptr;
  other.set_crash_dump_path_ = nullptr;
  other.set_process_delay_ = nullptr;
  other.free_parameters_ = nullptr;
  other.create_vfs_ = nullptr;
  other.disconnect_vfs_ = nullptr;
  other.clear_virtual_mappings_ = nullptr;
  other.virtual_link_directory_static_ = nullptr;
  other.virtual_link_file_ = nullptr;
  other.create_process_hooked_ = nullptr;
  other.blacklist_executable_ = nullptr;
  other.clear_executable_blacklist_ = nullptr;
  other.add_skip_file_suffix_ = nullptr;
  other.clear_skip_file_suffixes_ = nullptr;
  other.add_skip_directory_ = nullptr;
  other.clear_skip_directories_ = nullptr;
  other.force_load_library_ = nullptr;
  other.clear_library_force_loads_ = nullptr;
  other.init_logging_ = nullptr;
  other.get_log_messages_ = nullptr;
  other.get_vfs_process_list2_ = nullptr;
  other.update_parameters_ = nullptr;
  other.create_mini_dump_ = nullptr;
  other.log_level_to_string_ = nullptr;
  other.crash_dump_type_to_string_ = nullptr;
}

UsvfsLibrary &UsvfsLibrary::operator=(UsvfsLibrary &&other) noexcept {
  if (this != &other) {
    if (handle_) {
      FreeLibrary(handle_);
    }
    handle_ = other.handle_;
    loaded_ = other.loaded_;
    create_parameters_ = other.create_parameters_;
    set_instance_name_ = other.set_instance_name_;
    set_debug_mode_ = other.set_debug_mode_;
    set_log_level_ = other.set_log_level_;
    set_crash_dump_type_ = other.set_crash_dump_type_;
    set_crash_dump_path_ = other.set_crash_dump_path_;
    set_process_delay_ = other.set_process_delay_;
    free_parameters_ = other.free_parameters_;
    create_vfs_ = other.create_vfs_;
    disconnect_vfs_ = other.disconnect_vfs_;
    clear_virtual_mappings_ = other.clear_virtual_mappings_;
    virtual_link_directory_static_ = other.virtual_link_directory_static_;
    virtual_link_file_ = other.virtual_link_file_;
    create_process_hooked_ = other.create_process_hooked_;
    blacklist_executable_ = other.blacklist_executable_;
    clear_executable_blacklist_ = other.clear_executable_blacklist_;
    add_skip_file_suffix_ = other.add_skip_file_suffix_;
    clear_skip_file_suffixes_ = other.clear_skip_file_suffixes_;
    add_skip_directory_ = other.add_skip_directory_;
    clear_skip_directories_ = other.clear_skip_directories_;
    force_load_library_ = other.force_load_library_;
    clear_library_force_loads_ = other.clear_library_force_loads_;
    init_logging_ = other.init_logging_;
    get_log_messages_ = other.get_log_messages_;
    get_vfs_process_list2_ = other.get_vfs_process_list2_;
    update_parameters_ = other.update_parameters_;
    create_mini_dump_ = other.create_mini_dump_;
    log_level_to_string_ = other.log_level_to_string_;
    crash_dump_type_to_string_ = other.crash_dump_type_to_string_;

    other.handle_ = nullptr;
    other.loaded_ = false;
    other.create_parameters_ = nullptr;
    other.set_instance_name_ = nullptr;
    other.set_debug_mode_ = nullptr;
    other.set_log_level_ = nullptr;
    other.set_crash_dump_type_ = nullptr;
    other.set_crash_dump_path_ = nullptr;
    other.set_process_delay_ = nullptr;
    other.free_parameters_ = nullptr;
    other.create_vfs_ = nullptr;
    other.disconnect_vfs_ = nullptr;
    other.clear_virtual_mappings_ = nullptr;
    other.virtual_link_directory_static_ = nullptr;
    other.virtual_link_file_ = nullptr;
    other.create_process_hooked_ = nullptr;
    other.blacklist_executable_ = nullptr;
    other.clear_executable_blacklist_ = nullptr;
    other.add_skip_file_suffix_ = nullptr;
    other.clear_skip_file_suffixes_ = nullptr;
    other.add_skip_directory_ = nullptr;
    other.clear_skip_directories_ = nullptr;
    other.force_load_library_ = nullptr;
    other.clear_library_force_loads_ = nullptr;
    other.init_logging_ = nullptr;
    other.get_log_messages_ = nullptr;
    other.get_vfs_process_list2_ = nullptr;
    other.update_parameters_ = nullptr;
    other.create_mini_dump_ = nullptr;
    other.log_level_to_string_ = nullptr;
    other.crash_dump_type_to_string_ = nullptr;
  }
  return *this;
}

bool UsvfsLibrary::load() {
  // Controller DLL matching GMM arch — usvfs_x64.dll for 64-bit GMM.exe.
  handle_ = LoadLibraryW(L"usvfs_x64.dll");
  if (!handle_) {
    DWORD err = GetLastError();
    Logger::instance().error(
        "Failed to load usvfs_x64.dll: " + format_last_error(err) + kAvHint);
    return false;
  }

  bool ok = true;
  ok &= resolve("usvfsCreateParameters", create_parameters_);
  ok &= resolve("usvfsSetInstanceName", set_instance_name_);
  ok &= resolve("usvfsSetDebugMode", set_debug_mode_);
  ok &= resolve("usvfsSetLogLevel", set_log_level_);
  ok &= resolve("usvfsSetCrashDumpType", set_crash_dump_type_);
  ok &= resolve("usvfsSetCrashDumpPath", set_crash_dump_path_);
  ok &= resolve("usvfsSetProcessDelay", set_process_delay_);
  ok &= resolve("usvfsFreeParameters", free_parameters_);
  ok &= resolve("usvfsCreateVFS", create_vfs_);
  ok &= resolve("usvfsDisconnectVFS", disconnect_vfs_);
  ok &= resolve("usvfsClearVirtualMappings", clear_virtual_mappings_);
  ok &= resolve("usvfsVirtualLinkDirectoryStatic",
                virtual_link_directory_static_);
  ok &= resolve("usvfsVirtualLinkFile", virtual_link_file_);
  ok &= resolve("usvfsCreateProcessHooked", create_process_hooked_);
  ok &= resolve("usvfsBlacklistExecutable", blacklist_executable_);
  ok &= resolve("usvfsClearExecutableBlacklist", clear_executable_blacklist_);
  ok &= resolve("usvfsAddSkipFileSuffix", add_skip_file_suffix_);
  ok &= resolve("usvfsClearSkipFileSuffixes", clear_skip_file_suffixes_);
  ok &= resolve("usvfsAddSkipDirectory", add_skip_directory_);
  ok &= resolve("usvfsClearSkipDirectories", clear_skip_directories_);
  ok &= resolve("usvfsForceLoadLibrary", force_load_library_);
  ok &= resolve("usvfsClearLibraryForceLoads", clear_library_force_loads_);
  ok &= resolve("usvfsInitLogging", init_logging_);
  ok &= resolve("usvfsGetLogMessages", get_log_messages_);
  ok &= resolve("usvfsGetVFSProcessList2", get_vfs_process_list2_);
  ok &= resolve("usvfsUpdateParameters", update_parameters_);
  ok &= resolve("usvfsCreateMiniDump", create_mini_dump_);
  ok &= resolve("usvfsLogLevelToString", log_level_to_string_);
  ok &= resolve("usvfsCrashDumpTypeToString", crash_dump_type_to_string_);

  if (!ok) {
    Logger::instance().error("UsvfsLibrary: one or more USVFS symbols missing "
                             "— DLL may be quarantined or"
                             " mismatched." +
                             std::string(kAvHint));
    FreeLibrary(handle_);
    handle_ = nullptr;
    loaded_ = false;
    return false;
  }

  loaded_ = true;
  Logger::instance().debug("UsvfsLibrary: loaded usvfs_x64.dll");
  return true;
}

template <typename T> bool UsvfsLibrary::resolve(const char *name, T &out) {
  FARPROC proc = GetProcAddress(handle_, name);
  if (!proc) {
    DWORD err = GetLastError();
    Logger::instance().error("GetProcAddress failed for " + std::string(name) +
                             ": " + format_last_error(err) + kAvHint);
    out = nullptr;
    return false;
  }
  out = reinterpret_cast<T>(proc);
  return true;
}

} // namespace engine

#else // !_WIN32 — no out-of-line definitions needed; header provides inline
      // stubs.

#endif // _WIN32
