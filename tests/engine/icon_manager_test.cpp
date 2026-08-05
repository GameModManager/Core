// Offscreen test for engine::IconManager: the single icon-resolution chain.
//
// Every logical icon key resolves through one order:
//   default mode:   theme icons dir -> bundled resources/icons -> system
//                   (QIcon::fromTheme) -> fugue base pack -> standardIcon
//   <pack> mode:    resources/icons/packs/<pack> -> bundled -> system -> fugue
//   system mode:    system (QIcon::fromTheme) -> bundled -> fugue
//
// The last two tiers always act as a safety net, so a key that nothing
// supplies returns null only when the caller also passes SP_CustomBase.
//
// Hermetic: offscreen platform, throwaway HOME/XDG_DATA_HOME, synthetic
// resources tree (no real app assets), no network.
#include "engine/theme/icon_manager.h"

#include <QApplication>
#include <QByteArray>
#include <QIcon>
#include <QStyle>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (cond)
        ++passes;
    else
        ++failures;
}

// 1x1 transparent PNG.
static const char* kPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";

static void write_png(const fs::path& p) {
    const QByteArray bytes = QByteArray::fromBase64(kPngB64);
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.constData(), bytes.size());
}

static bool icon_has_surface(const QIcon& icon) {
    return !icon.isNull() && !icon.pixmap(16, 16).isNull();
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    fs::path base = fs::temp_directory_path() / "gmm_icon_manager_test";
    fs::remove_all(base);
    fs::create_directories(base);

    const fs::path app_dir = base / "app";
    const fs::path resources = base / "resources";
    const fs::path data = base / "data";

    // Bundled app assets: resources/icons/<key>.png
    const fs::path bundled_dir = resources / "icons";
    fs::create_directories(bundled_dir);
    write_png(bundled_dir / "bundled-key.png");

    // Packs: fugue (base) + zeta (arbitrary user-facing pack).
    const fs::path packs = bundled_dir / "packs";
    fs::create_directories(packs / "fugue");
    fs::create_directories(packs / "zeta");
    write_png(packs / "fugue" / "fallback-key.png");
    write_png(packs / "zeta" / "pack-only.png");

    // A theme that ships its own icons dir (XDG_DATA_HOME search dir 1).
    qputenv("XDG_DATA_HOME", data.string().c_str());
    const fs::path theme_icons = data / "GameModManager" / "themes" / "MyTheme" / "icons";
    fs::create_directories(theme_icons);
    write_png(theme_icons / "theme-key.png");

    auto& mgr = engine::IconManager::instance();
    mgr.discover_packs(app_dir);

    // Pack discovery: sorted, fugue guaranteed present.
    const auto names = mgr.pack_names();
    check(names.size() == 2, "two packs discovered");
    check(!names.empty() && names[0] == "fugue" && names[1] == "zeta",
          "pack_names sorted, fugue first");

    // Default mode, no active theme: bundled tier supplies the app default.
    mgr.set_mode("default");
    mgr.set_current_theme("");
    check(icon_has_surface(mgr.resolve_icon("bundled-key")), "default: bundled asset resolves");

    // Fugue base pack is the always-on safety net.
    check(icon_has_surface(mgr.resolve_icon("fallback-key")),
          "default: fugue fallback supplies key absent elsewhere");

    // Pack mode: the selected pack overrides bundled/system/fugue.
    mgr.set_mode("zeta");
    check(icon_has_surface(mgr.resolve_icon("pack-only")), "pack mode: selected pack supplies key");
    mgr.set_mode("default");

    // Theme icons dir wins in default mode when a theme is active.
    mgr.set_current_theme("MyTheme");
    check(icon_has_surface(mgr.resolve_icon("theme-key")), "default: theme icons dir supplies key");
    check(!icon_has_surface(mgr.resolve_icon("pack-only")),
          "default: theme mode never consults packs");

    // The literal theme name "default" means "no theme icons".
    mgr.set_current_theme("default");
    check(!icon_has_surface(mgr.resolve_icon("theme-key")),
          "default: theme named 'default' skips the theme icons dir");
    mgr.set_current_theme("MyTheme");

    // System mode: theme + pack tiers are skipped entirely.
    mgr.set_mode("system");
    check(!icon_has_surface(mgr.resolve_icon("theme-key")), "system: theme icons dir skipped");
    check(!icon_has_surface(mgr.resolve_icon("pack-only")), "system: packs skipped");
    check(icon_has_surface(mgr.resolve_icon("bundled-key")),
          "system: bundled asset still acts as the net");
    check(icon_has_surface(mgr.resolve_icon("fallback-key")), "system: fugue fallback still acts");
    mgr.set_mode("default");

    // Standard-icon fallback is the caller's last resort.
    check(icon_has_surface(mgr.resolve_icon("gmm-no-such-key", QStyle::SP_FileDialogInfoView)),
          "standardIcon fallback fires when everything else misses");
    check(!icon_has_surface(mgr.resolve_icon("gmm-no-such-key", QStyle::SP_CustomBase)),
          "SP_CustomBase means no fallback: unresolved key is null");

    fs::remove_all(base);

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
