// Checkerboard transparency grid (PreviewWindow / ImageViewer):
// style tiles match the ImageDiff reference colors, menu icons render 2x2
// swatches, transparency detection gates the grid, Settings persists the
// choice.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME.
#include "ui/preview/preview_widget.h"
#include "ui/settings/settings.h"

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QPixmap>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>

namespace {

QColor tile_pixel(const QPixmap& pm, int x, int y) {
  return pm.toImage().pixelColor(x, y);
}

}  // namespace

TEST_CASE("checkerboard transparency grid", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_checkerboard/config";
  std::filesystem::remove_all("/tmp/gmm_checkerboard");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  using ui::preview::CheckerboardStyle;

  SECTION("tiles match the reference colors") {
    const QPixmap light =
        ui::preview::checker_pixmap_for_style(CheckerboardStyle::Light);
    REQUIRE(light.width() == 16);
    REQUIRE(light.height() == 16);
    CHECK(tile_pixel(light, 0, 0) == QColor(204, 204, 204));
    CHECK(tile_pixel(light, 8, 0) == QColor(153, 153, 153));
    CHECK(tile_pixel(light, 0, 8) == QColor(153, 153, 153));
    CHECK(tile_pixel(light, 8, 8) == QColor(204, 204, 204));

    const QPixmap medium =
        ui::preview::checker_pixmap_for_style(CheckerboardStyle::Medium);
    CHECK(tile_pixel(medium, 0, 0) == QColor(153, 153, 153));
    CHECK(tile_pixel(medium, 8, 0) == QColor(102, 102, 102));

    const QPixmap dark = ui::preview::checker_pixmap_for_style(CheckerboardStyle::Dark);
    CHECK(tile_pixel(dark, 0, 0) == QColor(102, 102, 102));
    CHECK(tile_pixel(dark, 8, 0) == QColor(51, 51, 51));

    const QColor window_color = QApplication::palette().color(QPalette::Window);
    const QPixmap off = ui::preview::checker_pixmap_for_style(CheckerboardStyle::Off);
    CHECK(!off.isNull());
    CHECK(tile_pixel(off, 0, 0) == window_color);
    CHECK(tile_pixel(off, 8, 8) == window_color);
  }

  SECTION("menu icons render 2x2 swatches") {
    for (int style = 0; style <= 3; ++style) {
      const QIcon icon = ui::preview::checkerboard_icon(style);
      CHECK(!icon.isNull());
      CHECK(icon.pixmap(QSize(16, 16)).size() == QSize(16, 16));
    }
  }

  SECTION("transparency prefilter by extension") {
    CHECK(ui::preview::path_supports_transparency("tex/body.png"));
    CHECK(ui::preview::path_supports_transparency("anim/walk.anm2"));
    CHECK(ui::preview::path_supports_transparency("icon.WEBP"));
    CHECK_FALSE(ui::preview::path_supports_transparency("tex/body.jpg"));
    CHECK_FALSE(ui::preview::path_supports_transparency("tex/body.jpeg"));
    CHECK_FALSE(ui::preview::path_supports_transparency("readme.txt"));
  }

  SECTION("transparent pixels gate the grid") {
    QImage opaque_rgb(4, 4, QImage::Format_RGB32);
    opaque_rgb.fill(Qt::white);
    CHECK_FALSE(ui::preview::image_has_transparency(opaque_rgb));

    QImage opaque_argb(4, 4, QImage::Format_ARGB32);
    opaque_argb.fill(QColor(10, 20, 30, 255));
    CHECK_FALSE(ui::preview::image_has_transparency(opaque_argb));

    QImage with_hole(4, 4, QImage::Format_ARGB32);
    with_hole.fill(QColor(10, 20, 30, 255));
    with_hole.setPixelColor(2, 2, QColor(0, 0, 0, 0));
    CHECK(ui::preview::image_has_transparency(with_hole));

    CHECK_FALSE(ui::preview::image_has_transparency(QImage()));
  }

  SECTION("style persists in settings") {
    Settings& settings = Settings::instance();
    const int saved    = settings.checkerboard_style();
    settings.set_checkerboard_style(3);
    CHECK(settings.checkerboard_style() == 3);
    settings.set_checkerboard_style(99);
    CHECK(settings.checkerboard_style() == 3);  // clamped, write ignored
    settings.set_checkerboard_style(0);
    CHECK(settings.checkerboard_style() == 0);
    settings.set_checkerboard_style(saved);
  }
}
