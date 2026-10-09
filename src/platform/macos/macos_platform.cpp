#ifndef __APPLE__
#error "This file should only be compiled on the correct platform"
#endif

#include "platform/macos/macos_platform.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// LaunchServices + the CoreFoundation string/URL wrappers it takes. These are
// the only macOS frameworks the adaptor needs.
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>
#include <CoreServices/CoreServices.h>

namespace engine {

namespace {

  std::filesystem::path home_dir() {
    auto home = std::getenv("HOME");
    if (!home)
      return {};
    return std::filesystem::path(home);
  }

}  // namespace

// --- Library directory resolution ---

std::filesystem::path MacOSPlatform::data_dir() const {
  auto home = home_dir();
  if (home.empty())
    return "/tmp/GameModManager";
  return home / "Library" / "Application Support" / "GameModManager";
}

std::filesystem::path MacOSPlatform::config_dir() const {
  auto home = home_dir();
  if (home.empty())
    return "/tmp/GameModManager";
  return home / "Library" / "Application Support" / "GameModManager";
}

std::filesystem::path MacOSPlatform::cache_dir() const {
  auto home = home_dir();
  if (home.empty())
    return "/tmp/GameModManager";
  return home / "Library" / "Caches" / "GameModManager";
}

// --- Steam discovery ---

std::filesystem::path MacOSPlatform::find_steam_root() const {
  auto home = home_dir();
  if (home.empty())
    return {};

  auto root = home / "Library" / "Application Support" / "Steam";
  if (std::filesystem::exists(root)) {
    return root;
  }
  return {};
}

// Native macOS games keep user files in ~/Documents.
std::filesystem::path MacOSPlatform::native_documents_dir() const {
  auto home = home_dir();
  if (home.empty())
    return {};
  return home / "Documents";
}

std::filesystem::path MacOSPlatform::steam_userdata_dir() const {
  auto root = find_steam_root();
  if (root.empty())
    return {};
  return root / "userdata";
}

// --- Process launch ---

bool MacOSPlatform::launch_executable(const std::filesystem::path &executable,
                                      const std::vector<std::string> &args) const {
  if (!std::filesystem::exists(executable))
    return false;

  // Double-fork so the game process is reparented to launchd and never
  // becomes a zombie of this long-lived GUI process.
  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    setsid();
    pid_t inner = fork();
    if (inner == 0) {
      std::vector<char *> argv;
      argv.push_back(const_cast<char *>(executable.c_str()));
      for (const auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
      }
      argv.push_back(nullptr);
      execvp(executable.c_str(), argv.data());
      _exit(127);  // exec failed
    }
    _exit(inner < 0 ? 127 : 0);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// --- Privilege check ---

bool MacOSPlatform::is_elevated() const {
  return geteuid() == 0;
}

// --- Home / temp / thread priority ---

std::filesystem::path MacOSPlatform::home_dir() const {
  if (const char *home = std::getenv("HOME"); home && home[0] != '\0')
    return std::filesystem::path(home);
  return std::filesystem::temp_directory_path();
}

std::filesystem::path MacOSPlatform::temp_dir() const {
  return std::filesystem::temp_directory_path();
}

void MacOSPlatform::set_thread_low_priority() const {
  setpriority(PRIO_PROCESS, 0, 10);
}

// --- Protocol handler registration ---
//
// macOS has no per-user "make me the default for nxm://" call that works for a
// plain executable: LaunchServices identifies an app by its bundle id, and only
// a real .app carries one. So registration materializes a minimal bundle that
// wraps the running binary - Info.plist declaring the URL type, an executable
// symlink so the bundle tracks whatever build the user is on - and hands that
// bundle id to LSSetDefaultHandlerForURLScheme. Same binary, same behaviour as
// the other two adaptors, reached through the same interface.

namespace {

  struct SchemeSpec {
    const char *scheme;
    const char *role;  // Info.plist CFBundleTypeRole, descriptive only
  };

  SchemeSpec spec_for(ProtocolHandler protocol) {
    switch (protocol) {
    case ProtocolHandler::Nxm:
      return {"nxm", "Viewer"};
    case ProtocolHandler::Gmm:
      return {"gmm", "Viewer"};
    case ProtocolHandler::Modl:
      return {"modl", "Viewer"};
    }
    return {nullptr, nullptr};
  }

  std::string bundle_id_for(ProtocolHandler protocol) {
    const SchemeSpec spec = spec_for(protocol);
    return std::string("org.gamemodmanager.protocol.") +
           (spec.scheme ? spec.scheme : "");
  }

  std::filesystem::path bundle_path_for(ProtocolHandler protocol) {
    const SchemeSpec spec = spec_for(protocol);
    return std::filesystem::temp_directory_path() /
           ("GameModManager-" + std::string(spec.scheme ? spec.scheme : "") + ".app");
  }

  CFStringRef cf_string(const std::string &s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(),
                                     kCFStringEncodingUTF8);
  }

  // Narrow a CFString to UTF-8, empty on failure.
  std::string narrow(CFStringRef s) {
    if (!s)
      return {};
    const CFIndex cap = CFStringGetMaximumSizeForEncoding(kCFStringEncodingUTF8, s) + 1;
    std::string out(static_cast<std::size_t>(cap), '\0');
    const bool ok = CFStringGetCString(s, out.data(), cap, kCFStringEncodingUTF8);
    if (ok)
      out.resize(std::strlen(out.c_str()));
    return ok ? out : std::string{};
  }

}  // namespace

bool MacOSPlatform::register_protocol_handler(
    ProtocolHandler protocol, const std::filesystem::path &exe_path) const {
  const SchemeSpec spec = spec_for(protocol);
  if (!spec.scheme)
    return false;

  const auto bundle = bundle_path_for(protocol);
  std::error_code ec;
  std::filesystem::create_directories(bundle / "Contents" / "MacOS", ec);
  if (ec)
    return false;

  const auto link = bundle / "Contents" / "MacOS" / "GameModManager";
  std::filesystem::remove(link, ec);
  ec.clear();
  std::filesystem::create_symlink(exe_path, link, ec);
  if (ec)
    return false;

  const std::string scheme    = spec.scheme;
  const std::string bundle_id = bundle_id_for(protocol);

  {
    std::ofstream f(bundle / "Contents" / "Info.plist");
    if (!f)
      return false;
    f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n<dict>\n"
      << "<key>CFBundleExecutable</key><string>GameModManager</string>\n"
      << "<key>CFBundleIdentifier</key><string>" << bundle_id << "</string>\n"
      << "<key>CFBundleName</key><string>GameModManager</string>\n"
      << "<key>CFBundlePackageType</key><string>APPL</string>\n"
      << "<key>CFBundleURLTypes</key><array><dict>\n"
      << "<key>CFBundleURLName</key><string>org.gamemodmanager." << scheme
      << "</string>\n"
      << "<key>CFBundleURLSchemes</key><array><string>" << scheme
      << "</string></array>\n"
      << "<key>CFBundleTypeRole</key><string>" << spec.role << "</string>\n"
      << "</dict></array>\n"
      << "</dict>\n</plist>\n";
    if (!f.good())
      return false;
  }

  // Hand the bundle to LaunchServices, then claim the scheme for its id.
  CFStringRef path = cf_string(bundle.string());
  if (!path)
    return false;
  const CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, path,
                                                     kCFURLPOSIXPathStyle, true);
  CFRelease(path);
  if (!url)
    return false;
  const OSStatus reg = LSRegisterURL(url, true);
  CFRelease(url);
  if (reg != noErr)
    return false;

  CFStringRef bid   = cf_string(bundle_id_for(protocol));
  CFStringRef sch   = cf_string(spec.scheme);
  const OSStatus ok = LSSetDefaultHandlerForURLScheme(bid, sch);
  if (bid)
    CFRelease(bid);
  if (sch)
    CFRelease(sch);
  return ok == noErr;
}

bool MacOSPlatform::unregister_protocol_handler(ProtocolHandler protocol) const {
  if (!spec_for(protocol).scheme)
    return false;

  // Drop the claim first; whether LaunchServices accepts the reset is not
  // worth failing over, the bundle removal below is the part that must work.
  CFStringRef bid = cf_string(bundle_id_for(protocol));
  CFStringRef sch = cf_string(spec_for(protocol).scheme);
  if (bid && sch)
    LSSetDefaultHandlerForURLScheme(bid, sch);
  if (bid)
    CFRelease(bid);
  if (sch)
    CFRelease(sch);

  std::error_code ec;
  std::filesystem::remove_all(bundle_path_for(protocol), ec);
  return !ec;
}

bool MacOSPlatform::is_protocol_handler_registered(ProtocolHandler protocol) const {
  const SchemeSpec spec = spec_for(protocol);
  if (!spec.scheme)
    return false;
  CFStringRef sch     = cf_string(spec.scheme);
  CFStringRef handler = LSCopyDefaultHandlerForURLScheme(sch);
  if (sch)
    CFRelease(sch);
  if (!handler)
    return false;
  CFStringRef bid    = cf_string(bundle_id_for(protocol));
  const bool is_ours = bid && CFEqual(handler, bid);
  if (bid)
    CFRelease(bid);
  CFRelease(handler);
  return is_ours;
}

std::string MacOSPlatform::current_protocol_handler(ProtocolHandler protocol) const {
  const SchemeSpec spec = spec_for(protocol);
  if (!spec.scheme)
    return {};
  CFStringRef sch     = cf_string(spec.scheme);
  CFStringRef handler = LSCopyDefaultHandlerForURLScheme(sch);
  if (sch)
    CFRelease(sch);
  const std::string name = narrow(handler);
  if (handler)
    CFRelease(handler);
  return name;
}

// --- Free-function forms (see platform.h) ---

std::string platform_id() { return MacOSPlatform().platform_name(); }

std::filesystem::path find_steam_root() { return MacOSPlatform().find_steam_root(); }

std::filesystem::path default_cache_dir() { return MacOSPlatform().cache_dir(); }

std::filesystem::path find_wine() { return MacOSPlatform().find_wine(); }

bool register_protocol_handler(ProtocolHandler protocol,
                               const std::filesystem::path &exe_path) {
  return MacOSPlatform().register_protocol_handler(protocol, exe_path);
}

bool unregister_protocol_handler(ProtocolHandler protocol) {
  return MacOSPlatform().unregister_protocol_handler(protocol);
}

bool is_protocol_handler_registered(ProtocolHandler protocol) {
  return MacOSPlatform().is_protocol_handler_registered(protocol);
}

std::string current_protocol_handler(ProtocolHandler protocol) {
  return MacOSPlatform().current_protocol_handler(protocol);
}

// --- OS primitives that have no Platform instance to hang off ---

std::tm local_time(std::time_t t) {
  std::tm out{};
  if (!localtime_r(&t, &out))
    return {};
  return out;
}

std::tm utc_time(std::time_t t) {
  std::tm out{};
  if (!gmtime_r(&t, &out))
    return {};
  return out;
}

long current_process_id() {
  return static_cast<long>(getpid());
}

void set_thread_low_priority() {
  MacOSPlatform().set_thread_low_priority();
}

bool atomic_replace(const std::filesystem::path &from,
                    const std::filesystem::path &to) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  return !ec;
}

bool create_truncated_file(const std::filesystem::path &path, std::error_code &ec) {
  ec.clear();
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    ec.assign(errno, std::generic_category());
    return false;
  }
  ::close(fd);
  return true;
}

bool move_to_recycle_bin(const std::filesystem::path &path) {
  // macOS has no freedesktop trash: the Trash is a per-volume .Trashes
  // directory that Finder owns, and moving a file into it by rename is what
  // NSFileManager's trashItemAtURL does under the hood. .Trashes/501 is the
  // user (uid 501 on a stock macOS account) case Finder itself uses; the uid
  // is read rather than assumed so a non-501 account still lands correctly.
  const uid_t uid = getuid();
  const auto home = home_dir();
  if (home.empty())
    return false;

  // A volume-local trash is preferred (it is where Finder puts the file, so
  // "Put Back" and the Trash count in Finder both work), but it must be
  // writable by the user and the sticky-bit set, which only root can arrange
  // at the volume root. ~/ .Trash is always ours, so it is the fallback.
  std::vector<std::filesystem::path> candidates = {
      path.parent_path() / ".Trashes" / std::to_string(uid),
      home / ".Trash",
  };

  for (const auto &trash : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_directory(trash, ec))
      continue;
    if (::access(trash.c_str(), W_OK) != 0)
      continue;

    // Collision-free name, same shape as the Linux side.
    const auto base = path.filename();
    auto target     = trash / base;
    int n           = 0;
    for (;;) {
      ec.clear();
      if (!std::filesystem::exists(target, ec))
        break;
      target = trash / (base.string() + "." + std::to_string(++n));
    }

    ec.clear();
    std::filesystem::rename(path, target, ec);
    if (!ec)
      return true;

    // Cross-device (the volume-local trash lives on another filesystem than
    // a home-directory path): copy then remove.
    ec.clear();
    std::filesystem::copy(path, target,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::copy_symlinks,
                          ec);
    if (ec)
      continue;
    std::filesystem::remove_all(path, ec);
    if (!ec)
      return true;
  }
  return false;
}

bool path_is_executable(const std::filesystem::path &path) {
  return ::access(path.c_str(), X_OK) == 0;
}

bool filesystem_is_case_insensitive() {
  // APFS (and HFS+) fold case by default. A case-SENSITIVE APFS volume exists,
  // and this build cannot tell one from the other without probing the mount, so
  // the answer is the conservative one for a path index: a case-sensitive
  // index still resolves every path it can see on a case-insensitive volume,
  // while the reverse silently misses files.
  return false;
}

std::string machine_id() {
  // macOS has no /etc/machine-id and no registry. The hardware UUID lives in
  // IOPlatformUUID, reachable without IOKit through the ioreg command line;
  // shelled out rather than linked so src/platform/ stays framework-light.
  // Empty when it cannot be read, which is what the keyring treats as "no
  // machine binding available" and falls back to a per-install seed.
  std::unique_ptr<FILE, int (*)(FILE *)> pipe(
      ::popen("/usr/sbin/ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null", "r"),
      ::pclose);
  if (!pipe)
    return {};
  char line[512];
  while (std::fgets(line, sizeof(line), pipe.get())) {
    const std::string text(line);
    const auto key = text.find("\"IOPlatformUUID\"");
    if (key == std::string::npos)
      continue;
    const auto open  = text.find('"', text.find('=', key));
    const auto close = text.find('"', open + 1);
    if (open == std::string::npos || close == std::string::npos || close <= open + 1)
      continue;
    return text.substr(open + 1, close - open - 1);
  }
  return {};
}

std::optional<std::int64_t> file_birth_time(const std::filesystem::path &path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || st.st_birthtimespec.tv_sec == 0)
    return std::nullopt;
  return static_cast<std::int64_t>(st.st_birthtimespec.tv_sec);
}

int open_truncated_write_fd(const std::string &path) {
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

void write_raw_fd(int fd, const char *data, std::size_t size) {
  while (size > 0) {
    const auto n = ::write(fd, data, size);
    if (n <= 0)
      return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}

void close_raw_fd(int fd) {
  ::close(fd);
}

}  // namespace engine
