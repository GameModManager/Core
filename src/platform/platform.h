#pragma once

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace engine {

// URL schemes GameModManager can register itself as the system handler for.
// Every OS implements this differently, so nothing outside src/platform/ may
// name a concrete adaptor's registrars.
enum class ProtocolHandler {
  Nxm,  // nxm://  - Nexus Mods "download with mod manager" links
  Gmm,  // gmm://  - our own deep links
  Modl  // modl:// - mod.pub / MO2 modlhandler
};

// Platform abstraction layer. Each OS provides its own implementation.
// The engine never calls OS-specific APIs directly - always through this
// interface.
//
// Owned by main.cpp, passed to anything that needs platform services.
// Qt-free: lives in engine/, no Qt headers.
class Platform {
public:
  virtual ~Platform() = default;

  // Identity
  [[nodiscard]] virtual std::string platform_name() const = 0;

  // Directory resolution per XDG / Windows conventions
  [[nodiscard]] virtual std::filesystem::path data_dir() const   = 0;
  [[nodiscard]] virtual std::filesystem::path config_dir() const = 0;
  [[nodiscard]] virtual std::filesystem::path cache_dir() const  = 0;

  // Instances directory (under data_dir by default)
  [[nodiscard]] virtual std::filesystem::path instances_dir() const {
    return data_dir() / "instances";
  }

  // Steam discovery - platform-specific paths
  [[nodiscard]] virtual std::filesystem::path find_steam_root() const = 0;

  // A discovered Proton runner: user-facing name + path to the `proton`
  // script/binary. The name is what Steam/the user knows it as and what is
  // persisted as a per-instance runner override.
  struct ProtonVersionInfo {
    std::string name;
    std::filesystem::path binary;
  };

  // Proton discovery - returns path to proton script/binary, empty if
  // unavailable. On Windows this always returns empty (no Proton on Windows).
  // On Linux this searches Steam tools for Proton installations.
  [[nodiscard]] virtual std::filesystem::path find_proton() const { return {}; }

  // Per-game Proton runner selection, respecting Steam's per-game compat
  // tool override. Falls back to the latest Proton when no override exists.
  [[nodiscard]] virtual std::filesystem::path
  find_proton_for_game([[maybe_unused]] uint32_t steam_appid) const {
    return find_proton();
  }

  // Every installed Proton runner (name -> proton binary), for the UI's
  // runner selector. Empty on platforms without Proton.
  [[nodiscard]] virtual std::vector<ProtonVersionInfo>
  enumerate_proton_versions() const {
    return {};
  }

  // Resolve a named Proton runner (as persisted in instance.toml) to its
  // proton binary. `name` may be an absolute path (used directly) or the
  // runner's display name (searched among installed runners). Empty when
  // the runner cannot be found.
  [[nodiscard]] virtual std::filesystem::path
  find_proton_named([[maybe_unused]] const std::string &name) const {
    return {};
  }

  // Proton prefix (compatdata) directory for a game. Empty when not
  // applicable (no Steam, or the platform has no Proton).
  [[nodiscard]] virtual std::filesystem::path
  resolve_proton_prefix([[maybe_unused]] uint32_t steam_appid) const {
    return {};
  }

  // All Steam library folders (paths from libraryfolders.vdf, in priority
  // order). Empty when not applicable. Used to build
  // STEAM_COMPAT_LIBRARY_PATHS.
  [[nodiscard]] virtual std::vector<std::filesystem::path> steam_library_paths() const {
    return {};
  }

  // Windows user "Documents" directory for a game running under this
  // platform's prefix. On Linux this is inside the Proton prefix
  // (drive_c/users/<user>/Documents); on Windows the native
  // %USERPROFILE%\Documents. Empty when not applicable.
  [[nodiscard]] virtual std::filesystem::path
  game_documents_dir([[maybe_unused]] uint32_t steam_appid) const {
    return {};
  }

  // Host-native "Documents" directory: where a game running WITHOUT Proton
  // keeps its files. On Linux this honors ~/.config/user-dirs.dirs
  // (XDG_DOCUMENTS_DIR) with a ~/Documents fallback; on macOS it is
  // ~/Documents; on Windows it matches game_documents_dir(). Empty when the
  // host documents dir cannot be determined.
  [[nodiscard]] virtual std::filesystem::path native_documents_dir() const {
    return {};
  }

  // Steam "userdata" directory (<steam_install>/userdata/) holding per-user
  // cloud saves (<userid>/<appid>/remote/). Some games (Isaac) keep saves
  // here instead of Documents. Empty when Steam is not installed.
  [[nodiscard]] virtual std::filesystem::path steam_userdata_dir() const { return {}; }

  // Windows user "Local AppData" directory for a game running under this
  // platform's prefix. On Linux this is inside the Proton prefix
  // (drive_c/users/<user>/AppData/Local); on Windows the native
  // %LOCALAPPDATA%. Empty when not applicable.
  [[nodiscard]] virtual std::filesystem::path
  game_local_appdata_dir([[maybe_unused]] uint32_t steam_appid) const {
    return {};
  }

  // Wine discovery - for launching Windows games on Linux without Proton.
  // Returns path to wine binary, empty if unavailable.
  [[nodiscard]] virtual std::filesystem::path find_wine() const { return {}; }

  // --- Protocol handler registration ---
  //
  // GameModManager registers itself as the system handler for the schemes in
  // `ProtocolHandler` so "download with mod manager" links open here. The
  // mechanism differs per OS (XDG desktop files + xdg-mime, the
  // HKCU\Software\Classes registry, LaunchServices), so the UI goes through
  // these and never through a concrete adaptor.

  // Point `protocol` at the manager binary `exe_path`. False when the OS
  // refused the registration.
  [[nodiscard]] virtual bool
  register_protocol_handler(ProtocolHandler protocol,
                            const std::filesystem::path &exe_path) const = 0;

  // Hand `protocol` back to the OS default.
  [[nodiscard]] virtual bool
  unregister_protocol_handler(ProtocolHandler protocol) const = 0;

  // True when GameModManager is the current default handler for `protocol`.
  [[nodiscard]] virtual bool
  is_protocol_handler_registered(ProtocolHandler protocol) const = 0;

  // Human-readable name of whatever currently handles `protocol`, for the
  // "current handler: <name>" line. Empty when it cannot be determined.
  [[nodiscard]] virtual std::string
  current_protocol_handler(ProtocolHandler protocol) const = 0;

  // Launch a game executable. Platform handles the actual process creation.
  [[nodiscard]] virtual bool
  launch_executable(const std::filesystem::path &executable,
                    const std::vector<std::string> &args = {}) const = 0;

  // Check if the current user has elevated/admin privileges.
  [[nodiscard]] virtual bool is_elevated() const { return false; }

  // Check if symlinks are available (requires privileges on Windows).
  [[nodiscard]] virtual bool symlinks_available() const { return true; }

  // Check if NTFS junctions are available (Windows only, always true there).
  [[nodiscard]] virtual bool junctions_available() const { return false; }

  // Home directory - Linux/macOS: $HOME, Windows: %USERPROFILE%
  [[nodiscard]] virtual std::filesystem::path home_dir() const = 0;

  // Temporary directory
  [[nodiscard]] virtual std::filesystem::path temp_dir() const = 0;

  // Lower the current thread's CPU priority
  virtual void set_thread_low_priority() const {}
};

// --- Free-function forms -----------------------------------------------------
//
// Code paths that hold no Platform pointer (static utilities, the crash
// handler, anything running before the app shell exists) must not re-implement
// OS logic - the duplicate Steam-root scans and the three wine path lists that
// used to live in engine/ came from exactly this gap. These three forward to
// the virtuals above and resolve to the one implementation CMake compiles for
// this OS.

// "linux", "windows" or "macos".
[[nodiscard]] std::string platform_id();

// Steam install root, validated by steamapps/libraryfolders.vdf. Empty when
// Steam is not installed.
[[nodiscard]] std::filesystem::path find_steam_root();

// Per-user cache dir. Same value as Platform::cache_dir().
[[nodiscard]] std::filesystem::path default_cache_dir();

// Wine binary for running Windows games without Proton. Same value as
// Platform::find_wine(); empty when Wine is not installed.
[[nodiscard]] std::filesystem::path find_wine();

// Protocol handler registration, free-function form. The UI holds no Platform
// pointer here, so these forward to the virtuals above exactly like
// find_steam_root() does - same forwarding, same "one implementation per OS"
// guarantee. They exist so the UI never names LinuxPlatform/WindowsPlatform.
[[nodiscard]] bool register_protocol_handler(ProtocolHandler protocol,
                                             const std::filesystem::path &exe_path);
[[nodiscard]] bool unregister_protocol_handler(ProtocolHandler protocol);
[[nodiscard]] bool is_protocol_handler_registered(ProtocolHandler protocol);
[[nodiscard]] std::string current_protocol_handler(ProtocolHandler protocol);

// Thread-safe local-time breakdown. localtime_r on Linux/macOS,
// localtime_s on Windows. Returns a zeroed tm when the conversion fails,
// which every caller here treats as "no usable time" rather than an error:
// they are formatting a filename or a timestamp, not scheduling anything.
[[nodiscard]] std::tm local_time(std::time_t t);

// Same for UTC. gmtime_r on Linux/macOS, gmtime_s on Windows.
[[nodiscard]] std::tm utc_time(std::time_t t);

// Current process id. Uniqueness is all the callers want (temp-file names,
// cgroup names), not any signal or wait semantics.
[[nodiscard]] long current_process_id();

// Rename `from` over `to`, replacing an existing `to`, atomically with
// respect to a reader of `to`: it sees either the whole old file or the whole
// new one, never a mix. POSIX rename() is already that; Windows needs
// MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, which
// additionally forces the rename out to disk before returning - so a power
// loss right after this call cannot resurrect the old contents.
[[nodiscard]] bool atomic_replace(const std::filesystem::path &from,
                                  const std::filesystem::path &to);

// Create `path` as an empty file, truncating an existing one. Reports why it
// failed through `ec` (std::generic_category): std::filesystem has no
// create-file operation and a bare std::ofstream cannot say why it failed,
// which is exactly the case the caller has to show the user.
[[nodiscard]] bool create_truncated_file(const std::filesystem::path &path,
                                         std::error_code &ec);

// Move `path` to the OS trash, so it stays recoverable: the Recycle Bin via
// SHFileOperationW on Windows, the freedesktop.org Trash spec on Linux, and the
// volume-local .Trashes/<uid> (falling back to ~/.Trash) on macOS. False when
// the OS refused, in which case nothing was removed.
[[nodiscard]] bool move_to_recycle_bin(const std::filesystem::path &path);

// True when `path` exists and the current user may execute it.
[[nodiscard]] bool path_is_executable(const std::filesystem::path &path);

// True when the OS filesystem this process runs on resolves paths without
// regard to case (NTFS and APFS do; ext4 and XFS do not). Callers building a
// path index have to know, because the index they build is only correct for
// one of the two behaviours.
[[nodiscard]] bool filesystem_is_case_insensitive();

// Stable per-machine identifier, used to bind locally-stored key material to
// this installation. Linux: /etc/machine-id then /var/lib/dbus/machine-id.
// Windows: the MachineGuid registry value. macOS: the IOPlatformUUID hardware
// UUID, read through ioreg. Empty when the OS offers none, which callers
// already treat as "no machine binding available" and fall back to a
// per-install seed.
[[nodiscard]] std::string machine_id();

// Creation time of `path` as Unix epoch seconds, when the filesystem records
// one (statx STATX_BTIME, GetFileTime, st_birthtimespec). nullopt when the
// filesystem has no birth time or the read failed; callers fall back to mtime.
[[nodiscard]] std::optional<std::int64_t>
file_birth_time(const std::filesystem::path &path);

// A raw, unbuffered file descriptor, as the logger needs it: the forked launch
// supervisor must append a line without taking the logger's mutex (a GUI
// thread could have held it at fork) and without a C++ stream object that the
// child inherited in an indeterminate state. Returns -1 when the file cannot
// be opened.
[[nodiscard]] int open_truncated_write_fd(const std::string &path);

// Best-effort write of the whole buffer to a descriptor from
// open_truncated_write_fd(). Partial writes and errors are both swallowed: a
// log line is not worth the recovery path, and the caller has no better
// place to report it than the log it is writing to.
void write_raw_fd(int fd, const char *data, std::size_t size);

// Close a descriptor from open_truncated_write_fd().
void close_raw_fd(int fd);

// Free-function form of Platform::set_thread_low_priority(), for callers that
// hold no Platform pointer (the archive extractor runs on a worker thread
// before the app shell exists).
void set_thread_low_priority();

// The absolute path of the running executable, or empty when the OS will not
// say. Windows: GetModuleFileNameW with a buffer that grows until the value
// fits, because a per-user install under a long profile can exceed MAX_PATH.
// macOS: _NSGetExecutablePath, which can return a path containing symlinks,
// then weakly_canonical. Linux: readlink of /proc/self/exe.
[[nodiscard]] std::filesystem::path current_executable_path();

// The current user's home directory, or empty when the environment does not
// name one. This is deliberately NOT Platform::home_dir(), which falls back to
// the temp directory: a caller that has to tell "no home configured" from "home
// is the temp directory" needs the distinction, and guessing wrong there would
// write files somewhere the user never chose.
[[nodiscard]] std::filesystem::path home_dir_or_empty();

// True when the current user may create files in `dir`. Asked as a permission
// question rather than answered by attempting a write, so a probe never leaves
// a byte behind. Windows: GetFileAttributesW, because POSIX access() semantics
// and W_OK do not exist there. Linux/macOS: access(dir, W_OK).
[[nodiscard]] bool path_is_writable(const std::filesystem::path &dir);

// The mount point that `p` lives on, or empty when the platform does not
// express one. macOS: walk up to the first ancestor that is a mount point, so
// the answer does not depend on the bundle sitting at a fixed depth below
// /Volumes. Linux: the same walk. Windows: GetVolumePathNameW. Callers that
// only ask on macOS (a mounted disk image is read-only) still get a real
// answer everywhere rather than an empty stub.
[[nodiscard]] std::filesystem::path volume_root_of(const std::filesystem::path &p);

// Start `executable` as a fully detached child process: its own session, so it
// survives this process exiting and is not taken down by a Ctrl-C aimed at
// our terminal; `work_dir` as its working directory when non-empty; stdin from
// the null device, so it can never inherit a terminal and block on input.
//
// `argv` is the argument vector as the child sees it; argv[0] is passed through
// verbatim and does NOT have to be `executable`. The two differ in real use: a
// Proton runner is invoked by its full path but reports itself to the game by
// its bare filename. `executable` is what gets executed, and is resolved
// through PATH when it names no path.
//
// When `shell_fallback` is set and exec fails, the same argv is retried through
// /bin/sh, which is what lets a script carrying no shebang still start. Returns
// the child pid, or -1 when the process could not be started. The caller must
// not wait on it: that is the whole contract, and an implementation that waits
// would hang the launch path for the lifetime of the game.
[[nodiscard]] std::int64_t
spawn_detached_process(const std::filesystem::path &executable,
                       const std::vector<std::string> &argv,
                       const std::filesystem::path &work_dir, bool shell_fallback);

// --- Shared libraries --------------------------------------------------------
//
// dlopen/dlsym/dlclose versus LoadLibraryExW/GetProcAddress/FreeLibrary, behind
// one spelling. This exists because two call sites each hand-rolled the
// Windows side, and one of them (plugin_loader.cpp) shipped four inline shims
// named dlopen, dlsym, dlclose and dlerror that shadowed the real symbols.

// Load the shared library at `path`, lazily bound and private to this process.
// Returns an opaque handle, or nullptr on failure. `dlerror_message()` then
// says why.
[[nodiscard]] void *load_shared_library(const std::filesystem::path &path);

// Look up an exported symbol by name. nullptr when absent or the handle is
// null.
[[nodiscard]] void *shared_library_symbol(void *handle, const char *name);

// Release a handle from load_shared_library(). Null-safe.
void unload_shared_library(void *handle) noexcept;

// Why the last load_shared_library() failed, or an empty string. Never null,
// because the callers log it directly.
[[nodiscard]] const char *dlerror_message();

// Install location this application's own installer recorded, or empty when
// nothing recorded one. Windows: the InstallLocation value under the
// uninstall subkey our installer writes. Linux/macOS: empty, because neither
// has a registry and neither install method (QtIFW, AppImage, Flatpak) records
// a location in one. An installer that writes a different subkey therefore
// reports as "not installed by the installer", which is a visible report rather
// than a silent wrong answer.
[[nodiscard]] std::filesystem::path recorded_install_location();

// Centralized home dir lookup for code paths without a Platform pointer.
inline std::filesystem::path safe_home_dir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH + 1];
  if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH))
    return std::filesystem::path(buf);
#else
  if (const char *home = std::getenv("HOME"))
    return std::filesystem::path(home);
#endif
  return std::filesystem::temp_directory_path();
}

}  // namespace engine
