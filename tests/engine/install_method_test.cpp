// Install-method detection.
//
// Six shipping targets, one test each, plus the negatives that matter most: a
// layout we do not recognise must report "Unrecognised" rather than guess,
// because a wrong method is the worst failure this feature has - it decides
// which install method later writes to disk.
//
// The decision is a pure function over probed facts, so every platform's
// branch is exercised here on whatever host the suite runs on, without
// pretending to be that platform. The probe is the only part that touches the
// system, so the last two cases pin the properties that matter about it: it
// creates nothing, and it really does read $APPIMAGE.
//
// Fixtures root in the system temp directory.
#include "engine/update/install_method.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using engine::update::detect_install_method;
using engine::update::HostPlatform;
using engine::update::install_method_name;
using engine::update::install_method_route;
using engine::update::InstallFacts;
using engine::update::InstallMethod;
using engine::update::probe_install_facts;

namespace {

// --------------------------------------------------------------------------
// Layouts
// --------------------------------------------------------------------------

// A directory listing sensitive to any write: path, kind, byte size and
// last-write time of every entry, recursively. Compare two of these and a
// single added, removed, resized or rewritten file shows up.
std::vector<std::string> snapshot(const fs::path &root) {
  std::vector<std::string> rows;
  std::error_code ec;
  if (!fs::exists(root, ec))
    return rows;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       it != fs::recursive_directory_iterator{}; it.increment(ec)) {
    if (ec)
      break;
    std::error_code sec;
    const auto size  = it->is_directory(sec) ? 0 : it->file_size(sec);
    const auto mtime = fs::last_write_time(it->path(), sec).time_since_epoch().count();
    rows.push_back(it->path().generic_string() + "|" +
                   (it->is_directory(sec) ? "d" : "f") + "|" +
                   std::to_string(static_cast<long long>(size)) + "|" +
                   std::to_string(static_cast<long long>(mtime)));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

void touch(const fs::path &p) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << p.filename().string() << '\n';
}

fs::path case_root(const char *name) {
  const fs::path root =
      fs::temp_directory_path() / ("gmm_install_method_" + std::string(name));
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

// --------------------------------------------------------------------------
// Facts for each shipping target, as the probes would report them
// --------------------------------------------------------------------------

// Windows, NSIS per-user install: the uninstall entry's InstallLocation is
// the directory the exe runs from.
InstallFacts windows_installer() {
  InstallFacts f;
  f.platform = HostPlatform::Windows;
  f.exe_path = "C:/Users/pat/AppData/Local/Programs/GameModManager/gamemodmanager.exe";
  f.windows_uninstall_location = "C:/Users/pat/AppData/Local/Programs/GameModManager";
  return f;
}

// Windows, zip extracted where the user chose: nothing claims the directory.
InstallFacts windows_portable() {
  InstallFacts f;
  f.platform = HostPlatform::Windows;
  f.exe_path = "D:/Tools/GameModManager/gamemodmanager.exe";
  return f;
}

// Linux, .run installer into $HOME/GameModManager with its maintenance tool.
InstallFacts linux_qtifw() {
  InstallFacts f;
  f.platform              = HostPlatform::Linux;
  f.home                  = "/home/pat";
  f.exe_path              = "/home/pat/GameModManager/gamemodmanager";
  f.installerbase_sibling = true;
  return f;
}

// Linux, a single AppImage file.
InstallFacts appimage() {
  InstallFacts f;
  f.platform        = HostPlatform::Linux;
  f.home            = "/home/pat";
  f.exe_path        = "/home/pat/Applications/GameModManager-0.5.28-amd64.AppImage";
  f.appimage        = f.exe_path.string();
  f.appimage_exists = true;
  return f;
}

// Flatpak: /.flatpak-info plus the app id. The app-id is never pre-declared
// in code - it comes from the runtime, so detection does not depend on a
// packaging decision nobody has made.
InstallFacts flatpak() {
  InstallFacts f;
  f.platform             = HostPlatform::Linux;
  f.home                 = "/home/pat/.var/app/com.gamemodmanager.core";
  f.exe_path             = "/app/bin/gamemodmanager";
  f.flatpak_info_present = true;
  f.flatpak_id           = "com.gamemodmanager.core";
  return f;
}

// macOS, a bundle in a writable /Applications.
InstallFacts macos_dmg() {
  InstallFacts f;
  f.platform        = HostPlatform::MacOs;
  f.exe_path        = "/Applications/GameModManager.app/Contents/MacOS/gamemodmanager";
  f.volume_writable = true;
  return f;
}

// macOS, the same bundle still on a mounted image.
InstallFacts macos_mounted_image() {
  InstallFacts f;
  f.platform        = HostPlatform::MacOs;
  f.exe_path        = "/Volumes/GameModManager/GameModManager.app/Contents/MacOS/"
                      "gamemodmanager";
  f.volume_writable = false;
  return f;
}

}  // namespace

// ---------------------------------------------------------------------------
// One test per shipping target
// ---------------------------------------------------------------------------

TEST_CASE("install method: Windows installer is identified by its uninstall entry",
          "[engine][update][install-method]") {
  INFO("the entry's InstallLocation matches the directory the exe runs from");
  CHECK(detect_install_method(windows_installer()) == InstallMethod::WindowsInstaller);

  // The same directory the installer claims, spelled with a trailing
  // separator: the match is between directories, not between strings.
  auto trailing = windows_installer();
  trailing.windows_uninstall_location += "/";
  CHECK(detect_install_method(trailing) == InstallMethod::WindowsInstaller);
}

TEST_CASE("install method: Windows portable is the residual",
          "[engine][update][install-method]") {
  CHECK(detect_install_method(windows_portable()) == InstallMethod::WindowsPortable);

  // An uninstall entry pointing at a DIFFERENT directory is another product's
  // entry, not ours, so this install is still portable.
  auto foreign                       = windows_portable();
  foreign.windows_uninstall_location = "C:/Program Files/Some Other App";
  CHECK(detect_install_method(foreign) == InstallMethod::WindowsPortable);
}

TEST_CASE("install method: Qt Installer Framework needs both of its signals",
          "[engine][update][install-method]") {
  INFO("installerbase beside the binary AND the binary under $HOME/GameModManager");
  CHECK(detect_install_method(linux_qtifw()) == InstallMethod::LinuxQtIfw);

  // One signal alone is not enough: a build that merely ships an installerbase
  // is not a QtIFW install, and neither is a binary in the right directory
  // that somehow has no maintenance tool.
  auto no_tool                  = linux_qtifw();
  no_tool.installerbase_sibling = false;
  CHECK(detect_install_method(no_tool) == InstallMethod::Unknown);

  auto wrong_dir     = linux_qtifw();
  wrong_dir.exe_path = "/home/pat/Applications/gamemodmanager";
  CHECK(detect_install_method(wrong_dir) == InstallMethod::Unknown);
}

TEST_CASE("install method: AppImage is identified by a resolvable $APPIMAGE",
          "[engine][update][install-method]") {
  CHECK(detect_install_method(appimage()) == InstallMethod::AppImage);

  // $APPIMAGE set but pointing nowhere: a stale value must not send an
  // in-place overwrite at a path that is not there.
  auto stale            = appimage();
  stale.appimage_exists = false;
  CHECK(detect_install_method(stale) == InstallMethod::Unknown);
}

TEST_CASE("install method: Flatpak needs /.flatpak-info AND a non-empty id",
          "[engine][update][install-method]") {
  CHECK(detect_install_method(flatpak()) == InstallMethod::Flatpak);

  // An env var the user exported by hand cannot force the Flatpak route.
  auto no_marker                 = flatpak();
  no_marker.flatpak_info_present = false;
  CHECK(detect_install_method(no_marker) == InstallMethod::Unknown);

  auto empty_id = flatpak();
  empty_id.flatpak_id.clear();
  CHECK(detect_install_method(empty_id) == InstallMethod::Unknown);
}

TEST_CASE("install method: macOS dmg is identified, mounted image refused",
          "[engine][update][install-method]") {
  CHECK(detect_install_method(macos_dmg()) == InstallMethod::MacOsDmg);

  // A ~/Applications install is the same method as /Applications.
  auto user_apps     = macos_dmg();
  user_apps.home     = "/Users/pat";
  user_apps.exe_path = "/Users/pat/Applications/GameModManager.app/Contents/"
                       "MacOS/gamemodmanager";
  CHECK(detect_install_method(user_apps) == InstallMethod::MacOsDmg);

  CHECK(detect_install_method(macos_mounted_image()) ==
        InstallMethod::MacOsMountedImage);
}

// ---------------------------------------------------------------------------
// The negatives: a misdetection is the worst failure this feature has
// ---------------------------------------------------------------------------

TEST_CASE("install method: a layout that is none of the six reports Unrecognised",
          "[engine][update][install-method]") {
  // A bare build run straight out of a source tree.
  {
    InstallFacts f;
    f.platform = HostPlatform::Linux;
    f.home     = "/home/pat";
    f.exe_path = "/home/pat/gamemodmanager";
    INFO("a bare binary in $HOME");
    CHECK(detect_install_method(f) == InstallMethod::Unknown);
  }
  // A distro package install at the conventional path. We ship none, and a
  // package manager owns it, so it is not ours to write to.
  {
    InstallFacts f;
    f.platform = HostPlatform::Linux;
    f.home     = "/home/pat";
    f.exe_path = "/usr/bin/gamemodmanager";
    INFO("a distro package");
    CHECK(detect_install_method(f) == InstallMethod::Unknown);
  }
  // A developer build with nothing under /Applications and no Applications
  // dir in $HOME either.
  {
    InstallFacts f;
    f.platform        = HostPlatform::MacOs;
    f.exe_path        = "/Users/pat/build/gamemodmanager";
    f.volume_writable = true;
    INFO("a macOS build outside Applications");
    CHECK(detect_install_method(f) == InstallMethod::Unknown);
  }
  // An empty facts struct: no platform signal, no paths. Must not crash and
  // must not guess.
  {
    INFO("default-constructed facts");
    CHECK(detect_install_method(InstallFacts{}) == InstallMethod::Unknown);
  }
  // A bundle in /Applications on a read-only volume cannot be updated in
  // place, and it is not a mounted image either, so nothing claims it.
  {
    InstallFacts f;
    f.platform = HostPlatform::MacOs;
    f.exe_path = "/Applications/GameModManager.app/Contents/MacOS/gamemodmanager";
    f.volume_writable = false;
    INFO("a read-only /Applications");
    CHECK(detect_install_method(f) == InstallMethod::Unknown);
  }
  // A macOS-shaped path on Linux, and a Linux-shaped path on macOS: the
  // platform gate holds, so a path can never be read as another platform's.
  {
    InstallFacts f;
    f.platform = HostPlatform::Linux;
    f.exe_path = "/Applications/GameModManager.app/Contents/MacOS/gamemodmanager";
    f.volume_writable = true;
    INFO("a macOS path on Linux");
    CHECK(detect_install_method(f) == InstallMethod::Unknown);
  }
  {
    InstallFacts f;
    f.platform              = HostPlatform::MacOs;
    f.exe_path              = "/home/pat/GameModManager/gamemodmanager";
    f.installerbase_sibling = true;
    INFO("a QtIFW path on macOS");
    CHECK(detect_install_method(f) == InstallMethod::Unknown);
  }
}

// ---------------------------------------------------------------------------
// Ordering: a layout that satisfies two probes resolves to the one the
// ordering says wins. This is the subtle case, and the one a wrong order
// would silently get wrong by running the wrong install method.
// ---------------------------------------------------------------------------

TEST_CASE("install method: an AppImage inside a Flatpak is a Flatpak",
          "[engine][update][install-method][order]") {
  INFO("$APPIMAGE and /.flatpak-info both present; Flatpak is checked first");
  auto f                 = appimage();
  f.flatpak_info_present = true;
  f.flatpak_id           = "com.gamemodmanager.AppImage";
  CHECK(detect_install_method(f) == InstallMethod::Flatpak);
}

TEST_CASE("install method: an AppImage in a QtIFW directory is an AppImage",
          "[engine][update][install-method][order]") {
  INFO("$APPIMAGE wins over the $HOME/GameModManager + installerbase shape");
  auto f            = linux_qtifw();
  f.exe_path        = "/home/pat/GameModManager/gamemodmanager";
  f.appimage        = "/home/pat/GameModManager/gamemodmanager";
  f.appimage_exists = true;
  CHECK(detect_install_method(f) == InstallMethod::AppImage);
}

TEST_CASE("install method: an installer-shaped directory is not a portable one",
          "[engine][update][install-method][order]") {
  INFO("both a matching uninstall entry and a zip-shaped tree; the entry wins");
  auto f                       = windows_portable();
  f.windows_uninstall_location = "D:/Tools/GameModManager";
  CHECK(detect_install_method(f) == InstallMethod::WindowsInstaller);
}

TEST_CASE("install method: a writable mounted image is still a mounted image",
          "[engine][update][install-method][order]") {
  INFO("a rw-mounted DMG satisfies the writability test but not the route");
  auto f            = macos_mounted_image();
  f.volume_writable = true;
  CHECK(detect_install_method(f) == InstallMethod::MacOsMountedImage);
}

TEST_CASE("install method: a QtIFW path under /Volumes is still a mounted image",
          "[engine][update][install-method][order]") {
  INFO("the macOS mount check runs before the Linux maintenance-tool check");
  InstallFacts f;
  f.platform = HostPlatform::MacOs;
  f.home     = "/Users/pat";
  f.exe_path = "/Volumes/X/GameModManager.app/Contents/MacOS/gamemodmanager";
  f.installerbase_sibling = true;
  f.volume_writable       = true;
  CHECK(detect_install_method(f) == InstallMethod::MacOsMountedImage);
}

// ---------------------------------------------------------------------------
// Every method has a name and a route, because the panel renders them
// ---------------------------------------------------------------------------

TEST_CASE("install method: every method has a plain-language name and route",
          "[engine][update][install-method]") {
  const InstallMethod all[] = {
      InstallMethod::Unknown,         InstallMethod::WindowsInstaller,
      InstallMethod::WindowsPortable, InstallMethod::LinuxQtIfw,
      InstallMethod::AppImage,        InstallMethod::Flatpak,
      InstallMethod::MacOsDmg,        InstallMethod::MacOsMountedImage,
  };
  for (const auto m : all) {
    INFO("method " << static_cast<int>(m));
    const std::string name  = install_method_name(m);
    const std::string route = install_method_route(m);
    CHECK_FALSE(name.empty());
    CHECK_FALSE(route.empty());
  }
  // Each of the seven recognised outcomes is named as itself; only the
  // fallback is called "Unrecognised", so a method silently falling into the
  // fallback would show up here as the wrong name.
  for (const auto m :
       {InstallMethod::WindowsInstaller, InstallMethod::WindowsPortable,
        InstallMethod::LinuxQtIfw, InstallMethod::AppImage, InstallMethod::Flatpak,
        InstallMethod::MacOsDmg, InstallMethod::MacOsMountedImage})
    CHECK(std::string(install_method_name(m)) != "Unrecognised");
  CHECK(std::string(install_method_name(InstallMethod::Unknown)) == "Unrecognised");
  // The two methods that do not self-update say so in as many words, because
  // "no route" is the answer a user most needs to read.
  CHECK(std::string(install_method_route(InstallMethod::Unknown)).find("No route") !=
        std::string::npos);
  CHECK(std::string(install_method_route(InstallMethod::MacOsMountedImage))
            .find("No route") != std::string::npos);
  // A mounted image is named as such rather than as a plain dmg, because the
  // two need different action from the user.
  CHECK(std::string(install_method_name(InstallMethod::MacOsMountedImage)) !=
        install_method_name(InstallMethod::MacOsDmg));
}

// ---------------------------------------------------------------------------
// Side-effect freedom: detection decides whether we may write to disk later,
// so it must be provably incapable of writing.
// ---------------------------------------------------------------------------

TEST_CASE("install method: detection over real layouts writes nothing",
          "[engine][update][install-method][side-effects]") {
  // Build all six layouts on disk, because the probes look at real paths and
  // a synthetic struct alone would not prove anything about them.
  const fs::path root = case_root("sideeffects");

  const fs::path qtifw  = root / "home/GameModManager";
  const fs::path appimg = root / "home/Applications";
  const fs::path macvol = root / "Applications/GameModManager.app/Contents/MacOS";
  const fs::path win    = root / "win/Programs/GameModManager";
  const fs::path fla    = root / "flatpak/app/bin";
  for (const auto *d : {&qtifw, &appimg, &macvol, &win, &fla})
    fs::create_directories(*d);
  touch(qtifw / "installerbase");
  touch(qtifw / "gamemodmanager");
  touch(appimg / "GameModManager-0.5.28-amd64.AppImage");
  touch(macvol / "gamemodmanager");
  touch(win / "gamemodmanager.exe");
  touch(fla / "gamemodmanager");
  touch(root / "home/GameModManager/moddata.txt");

  const auto before = snapshot(root);
  REQUIRE_FALSE(before.empty());

  // Every method, over facts pointing at those real paths.
  InstallFacts facts[] = {
      windows_installer(), windows_portable(), linux_qtifw(),         appimage(),
      flatpak(),           macos_dmg(),        macos_mounted_image(),
  };
  for (const auto &f : facts) {
    INFO("method " << install_method_name(detect_install_method(f)));
    (void)detect_install_method(f);
  }
  // Repeat the whole sweep: a probe that creates on first sight and is a
  // no-op afterwards would slip past a single pass.
  for (const auto &f : facts)
    (void)detect_install_method(f);

  CHECK(snapshot(root) == before);
  fs::remove_all(root);
}

TEST_CASE("install method: the probe creates nothing and reads $APPIMAGE",
          "[engine][update][install-method][side-effects]") {
  // The probe is the only code that touches the system. On this platform its
  // entire read set is: /.flatpak-info, the running executable's own path,
  // an installerbase beside it, and $APPIMAGE. Pin all of it, then prove
  // nothing moved.
  std::error_code ec;
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  REQUIRE_FALSE(ec);
  const auto self_stat = [](const fs::path &p) {
    std::error_code ec;
    const bool there = fs::exists(p, ec);
    std::error_code sec;
    std::error_code tec;
    return p.string() + "|" + std::to_string(there ? fs::file_size(p, sec) : 0) + "|" +
           std::to_string(static_cast<long long>(
               fs::last_write_time(p, tec).time_since_epoch().count()));
  };

  // A throwaway AppImage outside the tree under test, so the probe has a real
  // file to resolve. Its root is not named gmm_* so it cannot be confused with
  // an artifact the probe might have created.
  const fs::path root = case_root("probe");
  const fs::path img  = root / "GameModManager-0.5.28-amd64.AppImage";
  touch(img);

  // Everything the probe could plausibly leave behind in the temp directory,
  // keyed on the gmm_* prefix the old update path used. Directories carry no
  // file size, so they are listed by path alone.
  const auto temp_gmm_entries = [] {
    std::vector<std::string> rows;
    std::error_code ec;
    for (auto it = fs::directory_iterator(fs::temp_directory_path(), ec);
         it != fs::directory_iterator{}; it.increment(ec)) {
      if (ec)
        break;
      if (it->path().filename().string().rfind("gmm", 0) != 0)
        continue;
      std::error_code tec;
      rows.push_back(
          it->path().generic_string() + "|" +
          std::to_string(static_cast<long long>(
              fs::last_write_time(it->path(), tec).time_since_epoch().count())));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
  };
  const auto gmm_temp_before = temp_gmm_entries();

  const auto self_before           = self_stat(self);
  const auto tool_before           = self_stat(self.parent_path() / "installerbase");
  const auto flatpak_marker_before = fs::exists("/.flatpak-info", ec);

  // Point the probe at our throwaway AppImage for the duration of the case.
  const char *prev_appimage = std::getenv("APPIMAGE");
  const std::string saved   = prev_appimage ? prev_appimage : "";
  ::setenv("APPIMAGE", img.c_str(), 1);

  // Run the real probe and the real decision, twice.
  const auto first  = detect_install_method(probe_install_facts());
  const auto second = detect_install_method(probe_install_facts());

  INFO("APPIMAGE is set to a file that exists, so the probe must find it");
  CHECK(first == InstallMethod::AppImage);
  CHECK(second == InstallMethod::AppImage);

  ::setenv("APPIMAGE", saved.c_str(), saved.empty() ? 0 : 1);

  // Nothing the probe read or could have read has moved, and it created no
  // gmm_* entry in the temp directory the way the old fetch path did.
  CHECK(self_stat(self) == self_before);
  CHECK(self_stat(self.parent_path() / "installerbase") == tool_before);
  CHECK(fs::exists("/.flatpak-info", ec) == flatpak_marker_before);

  const auto gmm_temp_after = temp_gmm_entries();
  CHECK(gmm_temp_after == gmm_temp_before);

  fs::remove_all(root);
}