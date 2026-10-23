// v3 P2 %VAR% path templates (Workspace-k8bc): the token grammar from the
// DX/VAR revision 1.1-1.6 and expansion against fake Platform adaptors for
// the four target columns of the 1.3 token table. Qt-free engine tests; the
// fake platforms stand in for the real adaptors so no Steam install or
// prefix is needed.
//
// Uses the check() PASS/FAIL pattern (Release builds compile out assert()).

#include "engine/game/registry/path_template.h"
#include "platform/platform.h"
#include "test_platform.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

using engine::PathTarget;
using gmm_test::InertPlatform;

namespace {

void check(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}

// Linux host carrying a Proton prefix: covers the "Linux native" column and,
// under a Windows target, the "Windows target under Proton" column.
class FakeLinuxPlatform : public InertPlatform {
public:
  fs::path home         = "/home/u";
  fs::path documents    = "/home/u/Documents";
  fs::path appdata      = "/home/u/.config";
  fs::path localappdata = "/home/u/.local/share";
  fs::path steam        = "/steam";
  fs::path prefix       = "/steam/steamapps/compatdata/892970";
  fs::path prefix_user =
      "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser";

  [[nodiscard]] std::string platform_name() const override { return "linux"; }
  [[nodiscard]] fs::path home_dir() const override { return home; }
  [[nodiscard]] fs::path native_documents_dir() const override { return documents; }
  [[nodiscard]] fs::path native_appdata_dir() const override { return appdata; }
  [[nodiscard]] fs::path native_localappdata_dir() const override {
    return localappdata;
  }
  [[nodiscard]] fs::path find_steam_root() const override { return steam; }
  [[nodiscard]] fs::path steam_userdata_dir() const override {
    return steam / "userdata";
  }
  [[nodiscard]] fs::path resolve_proton_prefix(uint32_t) const override {
    return prefix;
  }
  [[nodiscard]] fs::path game_userprofile_dir(uint32_t) const override {
    return prefix_user;
  }
  [[nodiscard]] fs::path game_documents_dir(uint32_t) const override {
    return prefix_user / "Documents";
  }
  [[nodiscard]] fs::path game_local_appdata_dir(uint32_t) const override {
    return prefix_user / "AppData" / "Local";
  }
};

// Native Windows host: the "Windows native" column.
class FakeWindowsPlatform : public InertPlatform {
public:
  fs::path home = "C:/Users/u";
  fs::path steam = "C:/Program Files/Steam";

  [[nodiscard]] std::string platform_name() const override { return "windows"; }
  [[nodiscard]] fs::path home_dir() const override { return home; }
  [[nodiscard]] fs::path native_documents_dir() const override {
    return home / "Documents";
  }
  [[nodiscard]] fs::path native_appdata_dir() const override {
    return home / "AppData" / "Roaming";
  }
  [[nodiscard]] fs::path native_localappdata_dir() const override {
    return home / "AppData" / "Local";
  }
  [[nodiscard]] fs::path native_locallow_dir() const override {
    return home / "AppData" / "LocalLow";
  }
  [[nodiscard]] fs::path native_programdata_dir() const override {
    return "C:/ProgramData";
  }
  [[nodiscard]] fs::path find_steam_root() const override { return steam; }
  [[nodiscard]] fs::path steam_userdata_dir() const override {
    return steam / "userdata";
  }
};

// Native macOS host: the "macOS native" column.
class FakeMacPlatform : public InertPlatform {
public:
  fs::path home  = "/Users/u";
  fs::path steam = "/Users/u/Library/Application Support/Steam";

  [[nodiscard]] std::string platform_name() const override { return "macos"; }
  [[nodiscard]] fs::path home_dir() const override { return home; }
  [[nodiscard]] fs::path native_documents_dir() const override {
    return home / "Documents";
  }
  [[nodiscard]] fs::path native_appdata_dir() const override {
    return home / "Library" / "Application Support";
  }
  [[nodiscard]] fs::path native_localappdata_dir() const override {
    return home / "Library" / "Application Support";
  }
  [[nodiscard]] fs::path native_programdata_dir() const override {
    return "/Library/Application Support";
  }
  [[nodiscard]] fs::path find_steam_root() const override { return steam; }
  [[nodiscard]] fs::path steam_userdata_dir() const override {
    return steam / "userdata";
  }
};

// Expand and stringify; an expansion error comes back as "<error: ...>" so a
// test can assert on it as one string.
std::string expand_str(const std::string &tmpl, const InertPlatform &platform,
                       PathTarget target, uint32_t appid = 0,
                       const fs::path &game_dir = "/games/valheim") {
  std::string error;
  const auto path =
      engine::expand_path_template(tmpl, &platform, target, appid, game_dir, error);
  if (!error.empty())
    return "<error:" + error + ">";
  return path.string();
}

}  // namespace

TEST_CASE("path template grammar", "[engine]") {
  // "100%" is safe: a lone % not followed by a token spelling stays literal.
  check(engine::validate_path_template("100%").empty(),
        "trailing lone % is literal, not a malformed token");
  check(engine::validate_path_template("%20/saves").empty(),
        "% followed by a digit is not a token start, stays literal");
  check(engine::validate_path_template("a%%b").empty(),
        "%% is a literal percent");
  check(engine::validate_path_template("%GAMEDIR%/x").empty(),
        "known token validates");
  check(!engine::validate_path_template("%BOGUS%/x").empty(),
        "unknown token name is rejected");
  check(engine::validate_path_template("%BOGUS%/x").find("BOGUS") !=
            std::string::npos,
        "the error names the bad token");
  check(!engine::validate_path_template("%LOCALLOW").empty(),
        "unterminated %-identifier is a malformed token, not a literal");
}

TEST_CASE("linux native column", "[engine]") {
  FakeLinuxPlatform platform;
  const uint32_t appid = 892970;
  check(expand_str("%GAMEDIR%/BepInEx", platform, PathTarget::Linux, appid) ==
            "/games/valheim/BepInEx",
        "GAMEDIR is the instance game dir");
  check(expand_str("%USERPROFILE%/x", platform, PathTarget::Linux, appid) ==
            "/home/u/x",
        "USERPROFILE is $HOME");
  check(expand_str("%DOCUMENTS%/x", platform, PathTarget::Linux, appid) ==
            "/home/u/Documents/x",
        "DOCUMENTS is native_documents_dir");
  check(expand_str("%APPDATA%/unity3d/IronGate/Valheim", platform,
                   PathTarget::Linux, appid) == "/home/u/.config/unity3d/IronGate/Valheim",
        "APPDATA is XDG_CONFIG_HOME or ~/.config");
  check(expand_str("%LOCALAPPDATA%/x", platform, PathTarget::Linux, appid) ==
            "/home/u/.local/share/x",
        "LOCALAPPDATA is XDG_DATA_HOME or ~/.local/share");
  check(expand_str("%STEAM%/x", platform, PathTarget::Linux, appid) == "/steam/x",
        "STEAM is the Steam root");
  check(expand_str("%STEAM_USERDATA%/892970/remote", platform, PathTarget::Linux,
                   appid) == "/steam/userdata/892970/remote",
        "STEAM_USERDATA is <steam>/userdata");
  // Undefined on Linux native: loud errors, never empty paths.
  check(expand_str("%LOCALLOW%/x", platform, PathTarget::Linux, appid)
                .find("<error:") == 0,
        "LOCALLOW is undefined on linux");
  check(expand_str("%PROGRAMDATA%/x", platform, PathTarget::Linux, appid)
                .find("<error:") == 0,
        "PROGRAMDATA is undefined on linux");
  check(expand_str("%WINEPREFIX%/x", platform, PathTarget::Linux, appid)
                .find("<error:") == 0,
        "WINEPREFIX is undefined on linux native");
}

TEST_CASE("windows native column", "[engine]") {
  FakeWindowsPlatform platform;
  check(expand_str("%USERPROFILE%/x", platform, PathTarget::Windows) ==
            "C:/Users/u/x",
        "USERPROFILE is the Windows profile");
  check(expand_str("%DOCUMENTS%/x", platform, PathTarget::Windows) ==
            "C:/Users/u/Documents/x",
        "DOCUMENTS is the native Documents dir");
  check(expand_str("%APPDATA%/x", platform, PathTarget::Windows) ==
            "C:/Users/u/AppData/Roaming/x",
        "APPDATA is the roaming dir");
  check(expand_str("%LOCALAPPDATA%/x", platform, PathTarget::Windows) ==
            "C:/Users/u/AppData/Local/x",
        "LOCALAPPDATA is the local dir");
  check(expand_str("%LOCALLOW%/IronGate/Valheim", platform, PathTarget::Windows) ==
            "C:/Users/u/AppData/LocalLow/IronGate/Valheim",
        "LOCALLOW is the LocalLow dir");
  check(expand_str("%PROGRAMDATA%/x", platform, PathTarget::Windows) ==
            "C:/ProgramData/x",
        "PROGRAMDATA is the machine-wide dir");
  check(expand_str("%WINEPREFIX%/x", platform, PathTarget::Windows)
                .find("<error:") == 0,
        "WINEPREFIX is undefined on native Windows");
}

TEST_CASE("macos native column", "[engine]") {
  FakeMacPlatform platform;
  check(expand_str("%USERPROFILE%/x", platform, PathTarget::MacOS) == "/Users/u/x",
        "USERPROFILE is $HOME");
  check(expand_str("%DOCUMENTS%/x", platform, PathTarget::MacOS) ==
            "/Users/u/Documents/x",
        "DOCUMENTS is ~/Documents");
  check(expand_str("%APPDATA%/IronGate/Valheim", platform, PathTarget::MacOS) ==
            "/Users/u/Library/Application Support/IronGate/Valheim",
        "APPDATA is ~/Library/Application Support");
  check(expand_str("%LOCALAPPDATA%/x", platform, PathTarget::MacOS) ==
            "/Users/u/Library/Application Support/x",
        "LOCALAPPDATA matches APPDATA on macOS");
  check(expand_str("%PROGRAMDATA%/x", platform, PathTarget::MacOS) ==
            "/Library/Application Support/x",
        "PROGRAMDATA is the read-only /Library probe");
  check(expand_str("%LOCALLOW%/x", platform, PathTarget::MacOS).find("<error:") ==
            0,
        "LOCALLOW is undefined on macOS");
}

TEST_CASE("windows target under proton resolves inside the prefix", "[engine]") {
  FakeLinuxPlatform platform;
  const uint32_t appid = 892970;
  check(expand_str("%USERPROFILE%/x", platform, PathTarget::Windows, appid) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser/x",
        "USERPROFILE is the prefix user dir");
  check(expand_str("%DOCUMENTS%/x", platform, PathTarget::Windows, appid) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser/"
            "Documents/x",
        "DOCUMENTS is the prefix Documents");
  check(expand_str("%APPDATA%/x", platform, PathTarget::Windows, appid) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser/"
            "AppData/Roaming/x",
        "APPDATA is the prefix roaming dir");
  check(expand_str("%LOCALAPPDATA%/x", platform, PathTarget::Windows, appid) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser/"
            "AppData/Local/x",
        "LOCALAPPDATA is the prefix local dir");
  check(expand_str("%LOCALLOW%/IronGate/Valheim", platform, PathTarget::Windows,
                   appid) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/users/steamuser/"
            "AppData/LocalLow/IronGate/Valheim",
        "LOCALLOW is the prefix LocalLow");
  check(expand_str("%PROGRAMDATA%/x", platform, PathTarget::Windows, appid) ==
            "/steam/steamapps/compatdata/892970/pfx/drive_c/ProgramData/x",
        "PROGRAMDATA is drive_c/ProgramData");
  check(expand_str("%WINEPREFIX%/x", platform, PathTarget::Windows, appid) ==
            "/steam/steamapps/compatdata/892970/x",
        "WINEPREFIX is the prefix root");
  check(expand_str("%STEAM%/x", platform, PathTarget::Windows, appid) ==
            "/steam/x",
        "STEAM stays the host Steam root under Proton");
}

TEST_CASE("phase B errors are loud and named", "[engine]") {
  FakeLinuxPlatform platform;
  // Known token, undefined for the target: error names token + target.
  const std::string error = expand_str("%LOCALLOW%/x", platform, PathTarget::Linux);
  check(error.find("LOCALLOW") != std::string::npos,
        "the error names the token");
  check(error.find("linux") != std::string::npos, "the error names the target");

  // Known token that resolves empty: same loudness, different reason.
  class NoSteamPlatform : public FakeLinuxPlatform {
  public:
    [[nodiscard]] fs::path find_steam_root() const override { return {}; }
  };
  NoSteamPlatform no_steam;
  const std::string steam_error =
      expand_str("%STEAM%/x", no_steam, PathTarget::Linux);
  check(steam_error.find("STEAM") != std::string::npos,
        "an empty resolution names the token");
}

TEST_CASE("no recursion and no expressions", "[engine]") {
  FakeLinuxPlatform platform;
  // A token's resolved value is not rescanned for %.
  class TrickyPlatform : public FakeLinuxPlatform {
  public:
    [[nodiscard]] fs::path home_dir() const override { return "/home/%USERPROFILE%"; }
  };
  TrickyPlatform tricky;
  check(expand_str("%USERPROFILE%/x", tricky, PathTarget::Linux) ==
            "/home/%USERPROFILE%/x",
        "a resolved value containing % is not expanded again");
}
