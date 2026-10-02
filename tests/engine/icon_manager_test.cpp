// Offscreen test for engine::IconManager: the single icon-resolution chain.
//
// Every logical icon key resolves through one order:
//   default mode:   theme icons dir -> bundled resources/icons -> system
//                   (QIcon::fromTheme) -> base pack (first discovered) -> standardIcon
//   <pack> mode:    resources/icons/packs/<pack> -> bundled -> system -> base pack
//   system mode:    system (QIcon::fromTheme) -> bundled -> base pack
//
// The last two tiers always act as a safety net, so a key that nothing
// supplies returns null only when the caller also passes SP_CustomBase.
//
// Hermetic: offscreen platform, throwaway HOME/XDG_DATA_HOME, synthetic
// resources tree (no real app assets), no network.
#include "engine/source/git/git_info.h"
#include "ui/theme/icon_manager.h"

#include <QApplication>
#include <QByteArray>
#include <QIcon>
#include <QStyle>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

// 1x1 transparent PNG.
static const char *kPngB64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42"
                             "mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

static void write_png(const fs::path &p) {
  const QByteArray bytes = QByteArray::fromBase64(kPngB64);
  std::ofstream out(p, std::ios::binary);
  out.write(bytes.constData(), bytes.size());
}

static bool icon_has_surface(const QIcon &icon) {
  return !icon.isNull() && !icon.pixmap(16, 16).isNull();
}

TEST_CASE("icon manager", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  fs::path base = fs::temp_directory_path() / "gmm_icon_manager_test";
  fs::remove_all(base);
  fs::create_directories(base);

  const fs::path app_dir   = base / "app";
  const fs::path resources = base / "resources";
  const fs::path data      = base / "data";

  // Bundled app assets: resources/icons/<key>.png
  const fs::path bundled_dir = resources / "icons";
  fs::create_directories(bundled_dir);
  write_png(bundled_dir / "bundled-key.png");

  // Branded source icons: resources/icons/vendor/<key>.ico
  const fs::path vendor_dir = bundled_dir / "vendor";
  fs::create_directories(vendor_dir);
  write_png(vendor_dir / "nexusmods.ico");
  write_png(vendor_dir / "loverslab.ico");

  // Packs: two arbitrary packs in a temp dir, discovered dynamically by the
  // manager - the sorted-first one ("alpha") is the base pack that the
  // tier-4 fallback uses. No pack name is hardcoded anywhere.
  const fs::path packs = bundled_dir / "packs";
  fs::create_directories(packs / "alpha");
  fs::create_directories(packs / "zeta");
  write_png(packs / "alpha" / "fallback-key.png");
  write_png(packs / "zeta" / "pack-only.png");

  // A theme that ships its own icons dir (XDG_DATA_HOME search dir 1).
  qputenv("XDG_DATA_HOME", data.string().c_str());
  const fs::path theme_icons = data / "GameModManager" / "themes" / "MyTheme" / "icons";
  fs::create_directories(theme_icons);
  write_png(theme_icons / "theme-key.png");

  auto &mgr = engine::IconManager::instance();
  mgr.discover_packs(app_dir);

  // Pack discovery: sorted, first pack is the base fallback.
  const auto names = mgr.pack_names();
  check(names.size() == 2, "two packs discovered");
  check(!names.empty() && names[0] == "alpha" && names[1] == "zeta",
        "pack_names sorted, base pack first");

  // Default mode, no active theme: bundled tier supplies the app default.
  mgr.set_mode("default");
  mgr.set_current_theme("");
  check(icon_has_surface(mgr.resolve_icon("bundled-key")),
        "default: bundled asset resolves");

  // The first discovered pack (base pack) is the always-on safety net; the
  // name comes from the packs/ listing, so renaming it keeps the fallback.
  check(icon_has_surface(mgr.resolve_icon("fallback-key")),
        "default: base-pack fallback supplies key absent elsewhere");
  check(!icon_has_surface(mgr.resolve_icon("pack-only")),
        "default: non-base packs are not the fallback");

  // Pack mode: the selected pack overrides bundled/system/base pack.
  mgr.set_mode("zeta");
  check(icon_has_surface(mgr.resolve_icon("pack-only")),
        "pack mode: selected pack supplies key");
  mgr.set_mode("default");

  // Theme icons dir wins in default mode when a theme is active.
  mgr.set_current_theme("MyTheme");
  check(icon_has_surface(mgr.resolve_icon("theme-key")),
        "default: theme icons dir supplies key");
  check(!icon_has_surface(mgr.resolve_icon("pack-only")),
        "default: theme mode never consults packs");

  // The literal theme name "default" means "no theme icons".
  mgr.set_current_theme("default");
  check(!icon_has_surface(mgr.resolve_icon("theme-key")),
        "default: theme named 'default' skips the theme icons dir");
  mgr.set_current_theme("MyTheme");

  // System mode: theme + pack tiers are skipped entirely.
  mgr.set_mode("system");
  check(!icon_has_surface(mgr.resolve_icon("theme-key")),
        "system: theme icons dir skipped");
  check(!icon_has_surface(mgr.resolve_icon("pack-only")), "system: packs skipped");
  check(icon_has_surface(mgr.resolve_icon("bundled-key")),
        "system: bundled asset still acts as the net");
  check(icon_has_surface(mgr.resolve_icon("fallback-key")),
        "system: base-pack fallback still acts");
  mgr.set_mode("default");

  // Standard-icon fallback is the caller's last resort.
  check(icon_has_surface(
            mgr.resolve_icon("gmm-no-such-key", QStyle::SP_FileDialogInfoView)),
        "standardIcon fallback fires when everything else misses");
  check(!icon_has_surface(mgr.resolve_icon("gmm-no-such-key", QStyle::SP_CustomBase)),
        "SP_CustomBase means no fallback: unresolved key is null");

  // Vendor tier: branded source icons resolve from resources/icons/vendor/.
  check(icon_has_surface(mgr.resolve_icon("nexusmods")),
        "default: vendor subdir supplies branded source icons");
  check(icon_has_surface(mgr.resolve_icon("loverslab")),
        "default: vendor subdir supplies loverslab");
  mgr.set_mode("system");
  check(icon_has_surface(mgr.resolve_icon("nexusmods")),
        "system: vendor icons still act as the app default net");
  mgr.set_mode("default");

  // vendor_icon_key mapping: source_type and display strings both map.
  check(engine::vendor_icon_key("nexus") == "nexusmods", "vendor_icon_key: nexus type");
  check(engine::vendor_icon_key("Nexus Mods") == "nexusmods",
        "vendor_icon_key: Nexus Mods display string");
  check(engine::vendor_icon_key("LoversLab") == "loverslab",
        "vendor_icon_key: LoversLab");
  check(engine::vendor_icon_key("steam") == "steam", "vendor_icon_key: steam type");
  check(engine::vendor_icon_key("Steam Workshop") == "steam",
        "vendor_icon_key: Steam Workshop display string");
  check(engine::vendor_icon_key("moddb") == "moddb", "vendor_icon_key: moddb");
  check(engine::vendor_icon_key("Manual").empty(),
        "vendor_icon_key: manual has no icon");
  check(engine::vendor_icon_key("").empty(), "vendor_icon_key: empty has no icon");

  // git_icon_key: the source_type is always "git", so the remote's HOST is the
  // only thing that picks a badge. github.com and gitlab.com are branded, and
  // a self-hosted Gitea is recognised by its first host label; every other
  // host - Bitbucket, a plain server, an unknown remote - is the generic git
  // badge, never a missing one.
  check(engine::git_icon_key("github.com") == "github",
        "git_icon_key: github.com gets the branded badge");
  check(engine::git_icon_key("www.github.com") == "github",
        "git_icon_key: www.github.com is the same host");
  check(engine::git_icon_key("GitHub.com") == "github",
        "git_icon_key: host match is case-insensitive");
  check(engine::git_icon_key("gitlab.com") == "gitlab",
        "git_icon_key: gitlab.com gets the branded badge");
  check(engine::git_icon_key("www.gitlab.com") == "gitlab",
        "git_icon_key: www.gitlab.com is the same host");
  check(engine::git_icon_key("gitea.example.com") == "gitea",
        "git_icon_key: a self-hosted gitea is recognised by its first label");
  check(engine::git_icon_key("GitEa.example.com") == "gitea",
        "git_icon_key: gitea label match is case-insensitive");
  check(engine::git_icon_key("gitea.com") == "gitea",
        "git_icon_key: the public gitea host");
  check(engine::git_icon_key("bitbucket.org") == "git",
        "git_icon_key: bitbucket falls back to generic git");
  check(engine::git_icon_key("notgitea.example.com") == "git",
        "git_icon_key: gitea only matches as the first label");
  check(engine::git_icon_key("git.example.com") == "git",
        "git_icon_key: self-hosted falls back to generic git");
  check(engine::git_icon_key("") == "git",
        "git_icon_key: no remote still resolves to the generic badge key");
  // Host extraction from the URL forms git actually writes.
  check(engine::Git::host_of("https://github.com/user/repo.git") == "github.com",
        "host_of: https URL");
  check(engine::Git::host_of("git@gitlab.com:group/proj.git") == "gitlab.com",
        "host_of: scp-like syntax");
  check(engine::Git::host_of("ssh://git@git.example.com:2222/p.git") ==
            "git.example.com",
        "host_of: ssh URL drops user and port");
  check(engine::Git::host_of("https://user:tok@BitBucket.org/a/b.git") ==
            "bitbucket.org",
        "host_of: embedded credentials and case are stripped");
  check(engine::Git::host_of("").empty(), "host_of: empty URL has no host");

  fs::remove_all(base);
}
