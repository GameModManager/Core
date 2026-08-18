// Test for theme base_style metadata: StyleManager applies the theme's
// declared Qt base style (e.g. "Fusion") before QSS so themes render
// consistently on platforms whose native style (Breeze, adwaita, ...) does
// not fully respect QSS.
//
// While a stylesheet is active, qApp->style() returns a QStyleSheetStyle
// wrapper; clearing the sheet exposes the underlying base style, which is
// what these assertions probe.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME.
#include "ui/theme/style_manager.h"
#include "engine/platform/theme/theme_manager.h"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

// Unwrap QStyleSheetStyle (active while a stylesheet is set) and return the
// underlying base style.
QStyle* base_style() {
    qApp->setStyleSheet(QString());
    return qApp->style();
}
}  // namespace

TEST_CASE("style manager base style", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const fs::path root = "/tmp/gmm_style_base_style";
    fs::remove_all(root);
    fs::create_directories(root / "themes" / "Dark");
    fs::create_directories(root / "themes" / "Plain");
    fs::create_directories(root / "themes" / "Bogus");
    qputenv("XDG_CONFIG_HOME", (root / "config").c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");

    write(root / "themes" / "Dark" / "dark.qss", "QWidget { color: #e6e6eb; }");
    write(root / "themes" / "Dark" / "theme.json", "{ \"base_style\": \"Fusion\" }");
    write(root / "themes" / "Plain" / "plain.qss", "QWidget { color: #000000; }");
    write(root / "themes" / "Bogus" / "bogus.qss", "QWidget { color: #123456; }");
    write(root / "themes" / "Bogus" / "theme.json", "{ \"base_style\": \"NoSuchStyle\" }");

    engine::ThemeManager theme_manager;
    theme_manager.scan_themes(root / "themes");
    engine::StyleManager style_manager(theme_manager);

    // A theme with base_style switches the app style to it before QSS.
    REQUIRE(style_manager.apply_theme("Dark"));
    REQUIRE(!qApp->styleSheet().isEmpty());
    REQUIRE(base_style()->inherits("QFusionStyle"));

    // A theme without base_style leaves the active style untouched.
    REQUIRE(style_manager.apply_theme("Plain"));
    REQUIRE(!qApp->styleSheet().isEmpty());
    REQUIRE(base_style()->inherits("QFusionStyle"));

    // An unavailable base style logs a warning and continues with the QSS.
    REQUIRE(style_manager.apply_theme("Bogus"));
    REQUIRE(!qApp->styleSheet().isEmpty());
    REQUIRE(base_style()->inherits("QFusionStyle"));

    // Default stays style-agnostic: it does not force a style.
    style_manager.apply_default();
    REQUIRE(!qApp->styleSheet().isEmpty());
    REQUIRE(base_style()->inherits("QFusionStyle"));
}