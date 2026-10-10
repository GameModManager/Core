#include "engine/update/install_method.h"

#include "platform/platform.h"

#include <cstdlib>
#include <cstring>

namespace engine::update {

namespace fs = std::filesystem;

namespace {

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

  bool path_exists(const fs::path &p) {
    if (p.empty())
      return false;
    std::error_code ec;
    return fs::exists(p, ec);
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

  // The host OS, as the adaptors name it. Every adaptor already answers this,
  // so the detector asks rather than branching on a compiler macro.
  const std::string os = engine::platform_id();
  if (os == "windows")
    f.platform = HostPlatform::Windows;
  else if (os == "macos")
    f.platform = HostPlatform::MacOs;
  else
    f.platform = HostPlatform::Linux;

  f.exe_path = engine::current_executable_path();
  f.home     = engine::home_dir_or_empty();

  const fs::path exe_dir = f.exe_path.parent_path();

  f.flatpak_info_present = path_exists("/.flatpak-info");
  f.flatpak_id           = env("FLATPAK_ID");

  f.appimage        = env("APPIMAGE");
  f.appimage_exists = path_exists(f.appimage);

  // QtIFW names the maintenance tool the same way on every platform it ships.
  f.installerbase_sibling = path_exists(exe_dir / "installerbase");

  // Both probes below are pure queries on every OS, so they run everywhere
  // rather than behind a platform branch. Only the macOS decision reads
  // volume_writable, and only the Windows decision reads
  // windows_uninstall_location, so asking on all three changes no outcome -
  // it just means neither answer depends on a preprocessor guess.
  f.volume_writable = engine::path_is_writable(engine::volume_root_of(exe_dir));
  f.windows_uninstall_location = engine::recorded_install_location().string();

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