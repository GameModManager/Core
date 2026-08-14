// Regression test for the frozen-UI-on-system-palette-change bug.
//
// GameModManager installs a global app stylesheet even for the Default
// (system) theme. Qt renders stylesheet-styled widgets through
// QStyleSheetStyle, which resolves palette(...) tokens at polish time and
// never re-evaluates them, so a KDE Light<->Dark switch left menubar text,
// buttons, inputs, the console and the right panel stuck on the old colors.
// StyleManager now re-applies the active sheet on QApplication::paletteChanged
// so every styled widget re-polishes against the new palette. RPGSaveEditor
// (the reference implementation) never sets a global stylesheet, so it needed
// no such workaround.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME.
#include "ui/theme/style_manager.h"
#include "engine/platform/theme/theme_manager.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QWidget>

#include <cstdio>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

TEST_CASE("style manager palette", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_style_palette/config";
    std::filesystem::remove_all("/tmp/gmm_style_palette");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");

    engine::ThemeManager theme_manager;
    engine::StyleManager style_manager(theme_manager);
    style_manager.apply_default();

    check(!qApp->styleSheet().isEmpty(),
          "default theme installs a global stylesheet");

    // Probe widget styled by a palette()-token QSS rule. #zoomControls is
    // `background-color: palette(midlight)` in default_qss, which
    // QStyleSheetStyle resolves at polish time into the widget's
    // QPalette::Window - and leaves stale there on a palette change unless the
    // sheet is re-applied.
    QWidget probe;
    probe.setObjectName("zoomControls");
    probe.resize(100, 40);
    probe.show();
    QApplication::processEvents();

    const QColor baseline_midlight = app.palette().color(QPalette::Midlight);
    check(probe.palette().color(QPalette::Window) == baseline_midlight,
          "probe rendered with palette(midlight) at baseline");

    // Simulate a system Light<->Dark switch: Qt propagates a new app palette
    // and StyleManager's ApplicationPaletteChange hook must re-polish the
    // probe so its palette()-token background follows the new palette.
    QPalette dark = app.palette();
    dark.setColor(QPalette::Midlight, QColor(0x12, 0x34, 0x56));
    dark.setColor(QPalette::Window, QColor(0x11, 0x11, 0x11));
    dark.setColor(QPalette::Base, QColor(0x22, 0x22, 0x22));
    dark.setColor(QPalette::Text, QColor(0xee, 0xee, 0xee));
    app.setPalette(dark);
    QApplication::processEvents();

    check(probe.palette().color(QPalette::Window) == dark.color(QPalette::Midlight),
          "probe re-resolves palette(midlight) after palette change");

    // The guard: a Qt built-in style clears the sheet (settings_dialog does
    // qApp->setStyleSheet(QString())); on such a palette change the sheet must
    // stay empty - the native path updates everything itself, StyleManager
    // must not clobber it back to default_qss.
    app.setStyleSheet(QString());
    app.setPalette(app.palette());
    QApplication::processEvents();
    check(qApp->styleSheet().isEmpty(),
          "empty-sheet guard: stylesheet stays cleared on palette change");
}
