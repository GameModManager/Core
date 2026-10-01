#include "engine/update/install_method.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace engine::update {

namespace fs = std::filesystem;

namespace {

  // The uninstall subkey our installer writes and the detector reads. Kept as
  // one constant on both sides of the contract: an installer that writes a
  // different subkey reports as WindowsPortable, which is a report, not an
  // install, so the failure mode is visible rather than silent.
  constexpr const char *kUninstallSubkey =
      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GameModManager";
  constexpr const char *kInstallLocationValue = "InstallLocation";

  // Env var lookup that distinguishes "absent" from "present but empty": both
  // mean "not this method", but only a non-empty value is a positive signal.
  std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
  }

  // True when child sits below parent, comparing one component at a time so
  // /Applications-other is not mistaken for /Applications.
  bool is_under(const fs::path &child, const fs::path &parent) {
    if (parent.empty())
      return false;
    auto c = child.begin();
    auto p = parent.begin();
    for (; p != parent.end(); ++p, ++c) {
      if (c == child.end() || *c != *p)
        return false;
    }
    return true;
  }

  // Drop trailing separators: "…/GameModManager/" and "…/GameModManager" are
  // the same directory, and lexically_normal keeps the difference, so
  // comparing the normalised forms alone would miss an install whose
  // InstallLocation carries a trailing backslash.
  fs::path without_trailing_sep(const fs::path &p) {
    fs::path out = p;
    // Stop at a root: "/" and "C:/" are a root, not a stray separator.
    while (out.filename().empty() && out.has_relative_path() && out != out.root_path())
      out = out.parent_path();
    return out;
  }

  // Two paths are the same directory when their normalised forms match. The
  // normalise step is what absorbs a trailing separator and "./" segments;
  // NSIS writes $INSTDIR verbatim, which is the same string the running exe
  // derives from its own path.
  bool same_dir(const fs::path &a, const fs::path &b) {
    if (a.empty() || b.empty())
      return false;
    return without_trailing_sep(a).lexically_normal() ==
           without_trailing_sep(b).lexically_normal();
  }

  // The running executable's path, or empty when the platform will not say.
  fs::path current_exe_path() {
#if defined(_WIN32)
    // MAX_PATH is 260 but a per-user install under a long profile can exceed
    // it, so grow the buffer until the value fits.
    std::wstring buf(512, L'\0');
    for (;;) {
      const DWORD n =
          GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
      if (n == 0)
        return {};
      if (n < buf.size()) {
        buf.resize(n);
        return fs::path(buf);
      }
      buf.resize(buf.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0)
      return {};
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
      return {};
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    return fs::weakly_canonical(fs::path(buf), ec);
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
      return {};
    return p;
#endif
  }

  fs::path home_path() {
#if defined(_WIN32)
    // USERPROFILE is what Windows sets for an interactive user; HOME is the
    // fallback some tooling (git bash, MSYS) sets instead.
    const std::string h = env("USERPROFILE").empty() ? env("HOME") : env("USERPROFILE");
    return h.empty() ? fs::path() : fs::path(h);
#else
    const std::string h = env("HOME");
    return h.empty() ? fs::path() : fs::path(h);
#endif
  }

  bool path_exists(const fs::path &p) {
    if (p.empty())
      return false;
    std::error_code ec;
    return fs::exists(p, ec);
  }

  // Writable is a permission question, asked with access(W_OK) rather than
  // answered by attempting a write: probing must not leave a byte behind.
  bool dir_writable(const fs::path &dir) {
    if (dir.empty())
      return false;
#if defined(_WIN32)
    // POSIX access() semantics do not exist here and W_OK is undefined for
    // directories. NT's AccessCheck over the directory handle is the honest
    // equivalent, and it is still a pure query.
    const DWORD attrs = GetFileAttributesW(dir.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
      return false;
    return true;
#else
    return ::access(dir.c_str(), W_OK) == 0;
#endif
  }

  // Walk up to the directory the bundle's volume lives in. On macOS a bundle
  // is /Applications/GameModManager.app/Contents/MacOS/gamemodmanager, and the
  // mount point is the first ancestor that is a mount point. Ask the kernel
  // rather than assume a depth.
  fs::path mounted_volume_root(const fs::path &p) {
#if defined(__APPLE__)
    std::error_code ec;
    for (auto dir = p; !dir.empty() && dir != dir.root_path();
         dir      = dir.parent_path()) {
      if (fs::is_mount_point(dir, ec))
        return dir;
      ec.clear();
    }
    return p;
#else
    (void)p;
    return {};
#endif
  }

}  // namespace

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------
InstallMethod detect_install_method(const InstallFacts &f) {
  // 1. Flatpak. Both halves must agree: /.flatpak-info is the marker that
  //    only a sandbox root has, $FLATPAK_ID says which app is running. A
  //    hand-set $FLATPAK_ID cannot reach this branch on its own, and this
  //    branch is deliberately ahead of the Linux probes so a sandbox that
  //    happens to ship an $APPIMAGE or a gamemodmanager.sh is still a Flatpak.
  if (f.flatpak_info_present && !f.flatpak_id.empty())
    return InstallMethod::Flatpak;

  // 2. AppImage. $APPIMAGE is set by the runtime that launched us and points
  //    at a file that must exist; the existence check stops a stale value from
  //    sending an in-place overwrite at the wrong path. Ahead of the other
  //    Linux probes for the same reason Flatpak is ahead of it.
  if (!f.appimage.empty() && f.appimage_exists)
    return InstallMethod::AppImage;

  // 3/4. Windows. The uninstall entry matching our own directory is the
  //      positive signal; portable is what is left over when it is absent.
  if (f.platform == HostPlatform::Windows) {
    if (!f.windows_uninstall_location.empty() &&
        same_dir(f.windows_uninstall_location, f.exe_path.parent_path()))
      return InstallMethod::WindowsInstaller;
    return InstallMethod::WindowsPortable;
  }

  // 5/6. macOS. A bundle under /Volumes is checked first and on purpose: even
  //      on a writable volume a mounted image is a case we refuse rather than
  //      write into, so it must not fall through to the installable case.
  if (f.platform == HostPlatform::MacOs) {
    const fs::path dir = f.exe_path.parent_path();
    if (is_under(dir, "/Volumes"))
      return InstallMethod::MacOsMountedImage;
    if (f.volume_writable &&
        (is_under(dir, "/Applications") ||
         (!f.home.empty() && is_under(dir, f.home / "Applications"))))
      return InstallMethod::MacOsDmg;
    return InstallMethod::Unknown;
  }

  // 7. Qt Installer Framework. Two agreeing signals, both from a real .run
  //    install: the binary sits at $HOME/GameModManager, and installerbase -
  //    the maintenance tool QtIFW installs by default - sits beside it. Either
  //    one alone would misidentify a build.
  if (f.installerbase_sibling && !f.home.empty() &&
      same_dir(f.home / "GameModManager", f.exe_path.parent_path()))
    return InstallMethod::LinuxQtIfw;

  return InstallMethod::Unknown;
}

// ---------------------------------------------------------------------------
// The probes
// ---------------------------------------------------------------------------
InstallFacts probe_install_facts() {
  InstallFacts f;

#if defined(_WIN32)
  f.platform = HostPlatform::Windows;
#elif defined(__APPLE__)
  f.platform = HostPlatform::MacOs;
#else
  f.platform = HostPlatform::Linux;
#endif

  f.exe_path = current_exe_path();
  f.home     = home_path();

  const fs::path exe_dir = f.exe_path.parent_path();

  f.flatpak_info_present = path_exists("/.flatpak-info");
  f.flatpak_id           = env("FLATPAK_ID");

  f.appimage        = env("APPIMAGE");
  f.appimage_exists = path_exists(f.appimage);

  // QtIFW names the maintenance tool the same way on every platform it ships.
  f.installerbase_sibling = path_exists(exe_dir / "installerbase");

#if defined(_WIN32)
  HKEY key = nullptr;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, kUninstallSubkey, 0, KEY_READ, &key) ==
      ERROR_SUCCESS) {
    char buf[1024] = {};
    DWORD type     = 0;
    DWORD size     = sizeof(buf) - 1;
    if (RegQueryValueExA(key, kInstallLocationValue, nullptr, &type,
                         reinterpret_cast<BYTE *>(buf), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
      f.windows_uninstall_location = buf;
    RegCloseKey(key);
  }
#elif defined(__APPLE__)
  f.volume_writable = dir_writable(mounted_volume_root(exe_dir));
#endif

  return f;
}

// ---------------------------------------------------------------------------
// Plain-language names and routes
// ---------------------------------------------------------------------------
const char *install_method_name(InstallMethod m) {
  switch (m) {
  case InstallMethod::WindowsInstaller:
    return "Windows installer";
  case InstallMethod::WindowsPortable:
    return "Windows portable folder";
  case InstallMethod::LinuxQtIfw:
    return "Qt Installer Framework";
  case InstallMethod::AppImage:
    return "AppImage";
  case InstallMethod::Flatpak:
    return "Flatpak";
  case InstallMethod::MacOsDmg:
    return "macOS disk image";
  case InstallMethod::MacOsMountedImage:
    return "macOS disk image, still mounted";
  case InstallMethod::Unknown:
    break;
  }
  return "Unrecognised";
}

const char *install_method_route(InstallMethod m) {
  switch (m) {
  case InstallMethod::WindowsInstaller:
    return "Updates re-run the downloaded Setup.exe, which owns this "
           "installation.";
  case InstallMethod::WindowsPortable:
    return "Updates are written straight into this folder.";
  case InstallMethod::LinuxQtIfw:
    return "Updates are handed to the installer's own maintenance tool, "
           "which needs an online installer build to have a repository.";
  case InstallMethod::AppImage:
    return "Updates replace this AppImage file in place.";
  case InstallMethod::Flatpak:
    return "Updates are requested from Flatpak itself, through the desktop "
           "portal.";
  case InstallMethod::MacOsDmg:
    return "Updates download a new disk image and replace this bundle in "
           "Applications.";
  case InstallMethod::MacOsMountedImage:
    return "No route: this copy is running from a mounted image, which is "
           "read-only, so it cannot replace itself. Run the copy in "
           "Applications instead.";
  case InstallMethod::Unknown:
    break;
  }
  return "No route: GameModManager cannot tell how this copy was installed, so "
         "it will not write to it. Reinstall from the download page to get "
         "updates.";
}

}  // namespace engine::update