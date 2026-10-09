#ifndef __APPLE__
#error "This file should only be compiled on the correct platform"
#endif

#include "platform/macos/macos_platform.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/resource.h>
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

}  // namespace engine
