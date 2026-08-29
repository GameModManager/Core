#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
// Minimal Windows type shims for non-Windows compilation (header must compile
// everywhere; implementation is Windows-only).
using HMODULE = void *;
using BOOL = int;
using DWORD = unsigned long;
using LPCWSTR = const wchar_t *;
using LPWSTR = wchar_t *;
using LPSTR = char *;
using LPCSTR = const char *;
using LPVOID = void *;
using HANDLE = void *;
using PEXCEPTION_POINTERS = void *;
using LPSECURITY_ATTRIBUTES = void *;
using LPSTARTUPINFOW = void *;
using LPPROCESS_INFORMATION = void *;
#ifndef WINAPI
#define WINAPI
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif

// USVFS shared types — must match references/usvfs/include/usvfs/logging.h
// and usvfsparameters.h. Defined globally so function pointers use the same
// types as the real DLL (global ::LogLevel / ::CrashDumpsType).
#ifndef GMM_USVFS_LOGLEVEL_DEFINED
#define GMM_USVFS_LOGLEVEL_DEFINED
enum class LogLevel : uint8_t { Debug, Info, Warning, Error };
#endif
#ifndef GMM_USVFS_CRASHDUMP_DEFINED
#define GMM_USVFS_CRASHDUMP_DEFINED
enum class CrashDumpsType : uint8_t { None, Mini, Data, Full };
#endif

struct usvfsParameters;

namespace engine {

// RAII wrapper for usvfs_x64.dll. Loads the controller DLL (matching GMM
// arch) via LoadLibraryW and resolves all required USVFS entry points via
// GetProcAddress. Logs LoadLibrary/GetProcAddress failures with an
// AV-quarantine hint. Unloads on destruction. Windows-only; on non-Windows
// the class is a stub that reports not loaded.
class UsvfsLibrary {
public:
#ifdef _WIN32
  UsvfsLibrary();
  ~UsvfsLibrary();
  UsvfsLibrary(const UsvfsLibrary &) = delete;
  UsvfsLibrary &operator=(const UsvfsLibrary &) = delete;
  UsvfsLibrary(UsvfsLibrary &&) noexcept;
  UsvfsLibrary &operator=(UsvfsLibrary &&) noexcept;
#else
  UsvfsLibrary() = default;
  ~UsvfsLibrary() = default;
  UsvfsLibrary(const UsvfsLibrary &) = delete;
  UsvfsLibrary &operator=(const UsvfsLibrary &) = delete;
  UsvfsLibrary(UsvfsLibrary &&) noexcept = default;
  UsvfsLibrary &operator=(UsvfsLibrary &&) noexcept = default;
#endif

  bool loaded() const { return loaded_; }

#ifdef _WIN32
  HMODULE handle() const { return handle_; }
#else
  HMODULE handle() const { return nullptr; }
#endif

  // Typed function pointer aliases (WINAPI / __stdcall on Windows)
  using CreateParametersFn = usvfsParameters *(WINAPI *)();
  using SetInstanceNameFn = void(WINAPI *)(usvfsParameters *, const char *);
  using SetDebugModeFn = void(WINAPI *)(usvfsParameters *, BOOL);
  using SetLogLevelFn = void(WINAPI *)(usvfsParameters *, ::LogLevel);
  using SetCrashDumpTypeFn = void(WINAPI *)(usvfsParameters *,
                                            ::CrashDumpsType);
  using SetCrashDumpPathFn = void(WINAPI *)(usvfsParameters *, const char *);
  using SetProcessDelayFn = void(WINAPI *)(usvfsParameters *, int);
  using FreeParametersFn = void(WINAPI *)(usvfsParameters *);
  using CreateVFSFn = BOOL(WINAPI *)(const usvfsParameters *);
  using DisconnectVFSFn = void(WINAPI *)();
  using ClearVirtualMappingsFn = void(WINAPI *)();
  using VirtualLinkDirectoryStaticFn = BOOL(WINAPI *)(LPCWSTR, LPCWSTR,
                                                      unsigned int);
  using VirtualLinkFileFn = BOOL(WINAPI *)(LPCWSTR, LPCWSTR, unsigned int);
  using CreateProcessHookedFn = BOOL(WINAPI *)(
      LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
      DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
  using BlacklistExecutableFn = void(WINAPI *)(LPCWSTR);
  using ClearExecutableBlacklistFn = void(WINAPI *)();
  using AddSkipFileSuffixFn = void(WINAPI *)(LPCWSTR);
  using ClearSkipFileSuffixesFn = void(WINAPI *)();
  using AddSkipDirectoryFn = void(WINAPI *)(LPCWSTR);
  using ClearSkipDirectoriesFn = void(WINAPI *)();
  using ForceLoadLibraryFn = void(WINAPI *)(LPCWSTR, LPCWSTR);
  using ClearLibraryForceLoadsFn = void(WINAPI *)();
  using InitLoggingFn = void(WINAPI *)(bool);
  using GetLogMessagesFn = bool(WINAPI *)(LPSTR, size_t, bool);
  using GetVFSProcessList2Fn = BOOL(WINAPI *)(size_t *, DWORD **);
  using UpdateParametersFn = void(WINAPI *)(usvfsParameters *);
  using CreateMiniDumpFn = int(WINAPI *)(PEXCEPTION_POINTERS, ::CrashDumpsType,
                                         const wchar_t *);
  using LogLevelToStringFn = const char *(WINAPI *)(::LogLevel);
  using CrashDumpTypeToStringFn = const char *(WINAPI *)(::CrashDumpsType);

  // Inline accessors — return resolved function pointers or nullptr if not
  // loaded.
#ifdef _WIN32
  CreateParametersFn create_parameters() const { return create_parameters_; }
  SetInstanceNameFn set_instance_name() const { return set_instance_name_; }
  SetDebugModeFn set_debug_mode() const { return set_debug_mode_; }
  SetLogLevelFn set_log_level() const { return set_log_level_; }
  SetCrashDumpTypeFn set_crash_dump_type() const {
    return set_crash_dump_type_;
  }
  SetCrashDumpPathFn set_crash_dump_path() const {
    return set_crash_dump_path_;
  }
  SetProcessDelayFn set_process_delay() const { return set_process_delay_; }
  FreeParametersFn free_parameters() const { return free_parameters_; }
  CreateVFSFn create_vfs() const { return create_vfs_; }
  DisconnectVFSFn disconnect_vfs() const { return disconnect_vfs_; }
  ClearVirtualMappingsFn clear_virtual_mappings() const {
    return clear_virtual_mappings_;
  }
  VirtualLinkDirectoryStaticFn virtual_link_directory_static() const {
    return virtual_link_directory_static_;
  }
  VirtualLinkFileFn virtual_link_file() const { return virtual_link_file_; }
  CreateProcessHookedFn create_process_hooked() const {
    return create_process_hooked_;
  }
  BlacklistExecutableFn blacklist_executable() const {
    return blacklist_executable_;
  }
  ClearExecutableBlacklistFn clear_executable_blacklist() const {
    return clear_executable_blacklist_;
  }
  AddSkipFileSuffixFn add_skip_file_suffix() const {
    return add_skip_file_suffix_;
  }
  ClearSkipFileSuffixesFn clear_skip_file_suffixes() const {
    return clear_skip_file_suffixes_;
  }
  AddSkipDirectoryFn add_skip_directory() const { return add_skip_directory_; }
  ClearSkipDirectoriesFn clear_skip_directories() const {
    return clear_skip_directories_;
  }
  ForceLoadLibraryFn force_load_library() const { return force_load_library_; }
  ClearLibraryForceLoadsFn clear_library_force_loads() const {
    return clear_library_force_loads_;
  }
  InitLoggingFn init_logging() const { return init_logging_; }
  GetLogMessagesFn get_log_messages() const { return get_log_messages_; }
  GetVFSProcessList2Fn get_vfs_process_list2() const {
    return get_vfs_process_list2_;
  }
  UpdateParametersFn update_parameters() const { return update_parameters_; }
  CreateMiniDumpFn create_mini_dump() const { return create_mini_dump_; }
  LogLevelToStringFn log_level_to_string() const {
    return log_level_to_string_;
  }
  CrashDumpTypeToStringFn crash_dump_type_to_string() const {
    return crash_dump_type_to_string_;
  }
#else
  CreateParametersFn create_parameters() const { return nullptr; }
  SetInstanceNameFn set_instance_name() const { return nullptr; }
  SetDebugModeFn set_debug_mode() const { return nullptr; }
  SetLogLevelFn set_log_level() const { return nullptr; }
  SetCrashDumpTypeFn set_crash_dump_type() const { return nullptr; }
  SetCrashDumpPathFn set_crash_dump_path() const { return nullptr; }
  SetProcessDelayFn set_process_delay() const { return nullptr; }
  FreeParametersFn free_parameters() const { return nullptr; }
  CreateVFSFn create_vfs() const { return nullptr; }
  DisconnectVFSFn disconnect_vfs() const { return nullptr; }
  ClearVirtualMappingsFn clear_virtual_mappings() const { return nullptr; }
  VirtualLinkDirectoryStaticFn virtual_link_directory_static() const {
    return nullptr;
  }
  VirtualLinkFileFn virtual_link_file() const { return nullptr; }
  CreateProcessHookedFn create_process_hooked() const { return nullptr; }
  BlacklistExecutableFn blacklist_executable() const { return nullptr; }
  ClearExecutableBlacklistFn clear_executable_blacklist() const {
    return nullptr;
  }
  AddSkipFileSuffixFn add_skip_file_suffix() const { return nullptr; }
  ClearSkipFileSuffixesFn clear_skip_file_suffixes() const { return nullptr; }
  AddSkipDirectoryFn add_skip_directory() const { return nullptr; }
  ClearSkipDirectoriesFn clear_skip_directories() const { return nullptr; }
  ForceLoadLibraryFn force_load_library() const { return nullptr; }
  ClearLibraryForceLoadsFn clear_library_force_loads() const { return nullptr; }
  InitLoggingFn init_logging() const { return nullptr; }
  GetLogMessagesFn get_log_messages() const { return nullptr; }
  GetVFSProcessList2Fn get_vfs_process_list2() const { return nullptr; }
  UpdateParametersFn update_parameters() const { return nullptr; }
  CreateMiniDumpFn create_mini_dump() const { return nullptr; }
  LogLevelToStringFn log_level_to_string() const { return nullptr; }
  CrashDumpTypeToStringFn crash_dump_type_to_string() const { return nullptr; }
#endif

private:
#ifdef _WIN32
  bool load();

  template <typename T> bool resolve(const char *name, T &out);

  HMODULE handle_ = nullptr;
  bool loaded_ = false;

  CreateParametersFn create_parameters_ = nullptr;
  SetInstanceNameFn set_instance_name_ = nullptr;
  SetDebugModeFn set_debug_mode_ = nullptr;
  SetLogLevelFn set_log_level_ = nullptr;
  SetCrashDumpTypeFn set_crash_dump_type_ = nullptr;
  SetCrashDumpPathFn set_crash_dump_path_ = nullptr;
  SetProcessDelayFn set_process_delay_ = nullptr;
  FreeParametersFn free_parameters_ = nullptr;
  CreateVFSFn create_vfs_ = nullptr;
  DisconnectVFSFn disconnect_vfs_ = nullptr;
  ClearVirtualMappingsFn clear_virtual_mappings_ = nullptr;
  VirtualLinkDirectoryStaticFn virtual_link_directory_static_ = nullptr;
  VirtualLinkFileFn virtual_link_file_ = nullptr;
  CreateProcessHookedFn create_process_hooked_ = nullptr;
  BlacklistExecutableFn blacklist_executable_ = nullptr;
  ClearExecutableBlacklistFn clear_executable_blacklist_ = nullptr;
  AddSkipFileSuffixFn add_skip_file_suffix_ = nullptr;
  ClearSkipFileSuffixesFn clear_skip_file_suffixes_ = nullptr;
  AddSkipDirectoryFn add_skip_directory_ = nullptr;
  ClearSkipDirectoriesFn clear_skip_directories_ = nullptr;
  ForceLoadLibraryFn force_load_library_ = nullptr;
  ClearLibraryForceLoadsFn clear_library_force_loads_ = nullptr;
  InitLoggingFn init_logging_ = nullptr;
  GetLogMessagesFn get_log_messages_ = nullptr;
  GetVFSProcessList2Fn get_vfs_process_list2_ = nullptr;
  UpdateParametersFn update_parameters_ = nullptr;
  CreateMiniDumpFn create_mini_dump_ = nullptr;
  LogLevelToStringFn log_level_to_string_ = nullptr;
  CrashDumpTypeToStringFn crash_dump_type_to_string_ = nullptr;
#else
  bool loaded_ = false;
#endif
};

} // namespace engine
