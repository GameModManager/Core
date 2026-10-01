#pragma once

#include <filesystem>
#include <string>

namespace engine::update {

// ---------------------------------------------------------------------------
// Install-method detection
// ---------------------------------------------------------------------------
// Which copy of the application is running, and therefore which update route
// could apply to it. The decision is split in two halves on purpose:
//
//   probe_install_facts()  reads the system - environment, two or three
//                          paths, one registry value. Read-only by
//                          construction: it stats and reads, never creates,
//                          writes or removes.
//   detect_install_method()  a pure function over the probed facts. No I/O at
//                          all, so every branch of it is testable on any
//                          platform without pretending to be another one.
//
// Nothing here looks at the executable's file name. "gamemodmanager" is
// identical across every target, so it carries no information.

enum class InstallMethod {
  Unknown,
  // Windows, installed by the NSIS installer. The installer writes an
  // uninstall registry entry whose InstallLocation is the directory the app
  // runs from; that match is what makes this definitive.
  WindowsInstaller,
  // Windows, a zip extracted into a directory the user chose. The residual
  // case, so it is only reached once WindowsInstaller has been ruled out.
  WindowsPortable,
  // Linux, installed by a Qt Installer Framework .run installer into
  // $HOME/GameModManager, which ships a sibling installerbase maintenance tool.
  LinuxQtIfw,
  // Linux, running from a single AppImage file. $APPIMAGE points at it.
  AppImage,
  // Flatpak deployment. /.flatpak-info is the marker; $FLATPAK_ID is the
  // second, independent half of the same signal. Neither alone is enough, so
  // an env var a user exports by hand cannot force this route.
  Flatpak,
  // macOS, a bundle in a writable /Applications. The updatable case.
  MacOsDmg,
  // macOS, a bundle still on a mounted .dmg. Split out from MacOsDmg because
  // the correct action here is a refusal, not an install: a mounted image is
  // read-only and has nowhere to put a replacement.
  MacOsMountedImage,
};

enum class HostPlatform {
  Linux,
  Windows,
  MacOs,
};

// Everything the decision is made from. Each field is the result of one
// read-only probe, which is why the decision function needs no platform
// conditionals of its own.
struct InstallFacts {
  HostPlatform platform = HostPlatform::Linux;
  // The running executable. Directories below are compared against its parent.
  std::filesystem::path exe_path;
  // The user's home directory, so $HOME/GameModManager can be recognised
  // without re-reading the environment inside the decision.
  std::filesystem::path home;

  // Flatpak: does /.flatpak-info exist, and what is $FLATPAK_ID?
  bool flatpak_info_present = false;
  std::string flatpak_id;

  // AppImage: $APPIMAGE, and whether that path actually resolves.
  std::string appimage;
  bool appimage_exists = false;

  // QtIFW: is there an installerbase next to the running binary?
  bool installerbase_sibling = false;

  // Windows: the InstallLocation value of our HKCU uninstall entry, empty
  // when the key is absent or names another product.
  std::string windows_uninstall_location;

  // macOS: is the volume holding the running bundle writable?
  bool volume_writable = false;
};

// The decision. Order-dependent, and the order is correctness rather than
// style: a path that satisfies two probes resolves to the one that comes
// first, and picking the other one runs the wrong install method.
//
//   Flatpak            before every Linux probe  - /.flatpak-info is definitive
//   AppImage           before every Linux probe  - $APPIMAGE is definitive
//   WindowsInstaller   before WindowsPortable    - portable is the residual
//   MacOsMountedImage  before MacOsDmg           - the action differs
//
// LinuxQtIfw and Unknown are last: QtIFW needs two agreeing signals, and
// Unknown is the only answer for a layout we do not recognise, which is
// always better than guessing.
[[nodiscard]] InstallMethod detect_install_method(const InstallFacts &facts);

// Read the facts from the running system. Cheap enough for a launch path and
// read-only: environment reads, path existence checks and one registry read.
// It decides whether anything may later be written, so it must be provably
// incapable of writing.
[[nodiscard]] InstallFacts probe_install_facts();

// Plain-language names for the UI, so the settings panel can say what this
// install is without the UI carrying a switch over InstallMethod.
[[nodiscard]] const char *install_method_name(InstallMethod method);

// Why that method does or does not self-update, in one sentence per method.
[[nodiscard]] const char *install_method_route(InstallMethod method);

}  // namespace engine::update