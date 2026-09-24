// PreviewWindow image auto-fit: small images scale UP to fill the viewport,
// large images scale DOWN so they are not cropped, and resizing the window
// re-fits the image.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME.
#include "ui/preview/preview_window.h"

#include <QApplication>
#include <QImage>
#include <QSize>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

bool aspect_preserved(const QSize &shown, const QSize &src) {
  if (shown.isEmpty() || src.isEmpty())
    return false;
  const double a = static_cast<double>(shown.width()) / shown.height();
  const double b = static_cast<double>(src.width()) / src.height();
  return std::abs(a - b) < 0.05;
}

}  // namespace

TEST_CASE("preview window auto-fits images", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_preview_fit/config";
  std::filesystem::remove_all("/tmp/gmm_preview_fit");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path img_dir = "/tmp/gmm_preview_fit/img";
  std::filesystem::create_directories(img_dir);

  const QSize small_src(32, 24);
  QImage small_img(small_src, QImage::Format_RGB32);
  small_img.fill(Qt::red);
  REQUIRE(
      small_img.save(QString::fromStdString((img_dir / "small.png").string()), "PNG"));

  const QSize large_src(2000, 1500);
  QImage large_img(large_src, QImage::Format_RGB32);
  large_img.fill(Qt::blue);
  REQUIRE(
      large_img.save(QString::fromStdString((img_dir / "large.png").string()), "PNG"));

  ui::preview::PreviewWindow window;

  // Small image scales UP to fill the viewport (aspect preserved).
  window.show_file(QString::fromStdString((img_dir / "small.png").string()));
  QApplication::processEvents();
  const QSize small_shown = window.displayed_pixmap_size();
  CHECK(small_shown.width() > small_src.width());
  CHECK(small_shown.height() > small_src.height());
  CHECK(aspect_preserved(small_shown, small_src));

  // Large image scales DOWN to fit the viewport (aspect preserved).
  window.show_file(QString::fromStdString((img_dir / "large.png").string()));
  QApplication::processEvents();
  const QSize large_shown = window.displayed_pixmap_size();
  CHECK(large_shown.width() < large_src.width());
  CHECK(large_shown.height() < large_src.height());
  CHECK(aspect_preserved(large_shown, large_src));

  // Resizing the window re-fits the image (bigger window, bigger display).
  window.resize(window.width() + 200, window.height() + 200);
  QApplication::processEvents();
  const QSize resized_shown = window.displayed_pixmap_size();
  CHECK(resized_shown.width() > large_shown.width());
  CHECK(resized_shown.height() > large_shown.height());
  CHECK(aspect_preserved(resized_shown, large_src));
}
