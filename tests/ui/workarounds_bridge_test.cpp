// Settings > Workarounds > Miscellaneous: the QSettings read must reach the
// engine, not just the store.
//
// The engine is Qt-free and cannot reach QSettings, so the two skip lists are
// handed over by Settings::apply_workarounds(). This case drives THAT bridge -
// write the key, call the bridge, then assert on the engine predicate the
// deploy / conflict / scanner walks already call. A test that only checked
// get_workarounds() would pass with the bridge body emptied; this one does not.
//
// Hermetic: offscreen platform, throwaway config (see tests/support/test_main.cpp).
#include "engine/core/util/fs_utils.h"
#include "ui/settings/settings.h"

#include <QApplication>

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the skip lists reach the engine predicates", "[ui][settings]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_workarounds_bridge/config";
  std::filesystem::remove_all("/tmp/gmm_workarounds_bridge");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int argc     = 1;
  char argv0[] = "test";
  char *argv[] = {argv0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &s = Settings::instance();

  SECTION("untouched: nothing is hidden and nothing is skipped") {
    s.set_skip_file_suffixes({});
    s.set_skip_directories({});
    s.apply_workarounds();
    CHECK_FALSE(engine::is_hidden_file("notes.skipme"));
    CHECK_FALSE(engine::is_skipped_directory(".git"));
  }

  SECTION("typed in the field: the engine honours it after the bridge") {
    // The field is free text, so the entries arrive with the surrounding
    // spaces a user typed. Trimming is part of the bridge: an untrimmed
    // ".skipme " would never match a filename.
    s.set_skip_file_suffixes({QStringLiteral("  .skipme  ")});
    s.set_skip_directories({QStringLiteral(" .git/ ")});
    s.apply_workarounds();
    CHECK(engine::is_hidden_file("notes.skipme"));
    CHECK(engine::is_skipped_directory(".git"));
  }

  SECTION("clearing the field again releases the engine") {
    s.set_skip_file_suffixes({QStringLiteral(".skipme")});
    s.set_skip_directories({QStringLiteral(".git")});
    s.apply_workarounds();
    REQUIRE(engine::is_hidden_file("notes.skipme"));

    s.set_skip_file_suffixes({});
    s.set_skip_directories({});
    s.apply_workarounds();
    CHECK_FALSE(engine::is_hidden_file("notes.skipme"));
    CHECK_FALSE(engine::is_skipped_directory(".git"));
  }

  s.set_skip_file_suffixes({});
  s.set_skip_directories({});
  s.apply_workarounds();
  std::filesystem::remove_all("/tmp/gmm_workarounds_bridge");
}
