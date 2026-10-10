// Last-used directory per named file dialog (MO2 FileDialogMemory,
// filedialogmemory.cpp). Two things can silently break here and both are
// invisible until a user hits Browse twice in a row:
//
//   * a dialog that WAS given an explicit start directory must keep it - the
//     remembered folder is a fallback, never an override;
//   * a cancelled dialog must not erase the memory, or the next open lands
//     somewhere random.
//
// The second half of the file covers the persistence: the map has to survive
// the two-parallel-QStringList encoding Settings uses, or the memory lasts one
// session.
#include "ui/settings/settings.h"
#include "ui/widgets/file_dialog_memory.h"

#include <QMap>
#include <QString>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("file dialog memory picks the start directory", "[ui][settings]") {
  using ui::FileDialogMemory;

  FileDialogMemory::remember("import-modlist", "/mods/dl");
  FileDialogMemory::remember("export-modlist", "/mods/out");

  SECTION("a dialog with no explicit directory opens where it last was") {
    REQUIRE(FileDialogMemory::start_dir("import-modlist", QString()) == "/mods/dl");
  }

  SECTION("the memory is per dialog, not one global folder") {
    REQUIRE(FileDialogMemory::start_dir("export-modlist", QString()) == "/mods/out");
    REQUIRE(FileDialogMemory::start_dir("import-modlist", QString()) == "/mods/dl");
  }

  SECTION("an explicit directory always wins over the memory") {
    REQUIRE(FileDialogMemory::start_dir("import-modlist", "/explicit") == "/explicit");
  }

  SECTION("an unknown dialog with no explicit directory starts nowhere") {
    REQUIRE(FileDialogMemory::start_dir("never-seen", QString()).isEmpty());
  }

  SECTION("a cancelled dialog leaves the memory alone") {
    // An empty path is what a cancelled dialog yields; it must not be stored,
    // or the next open of this dialog loses the folder it had.
    FileDialogMemory::remember("import-modlist", QString());
    REQUIRE(FileDialogMemory::start_dir("import-modlist", QString()) == "/mods/dl");
  }

  SECTION("an unnamed dialog is never remembered") {
    FileDialogMemory::remember(QString(), "/somewhere");
    REQUIRE(FileDialogMemory::start_dir(QString(), QString()).isEmpty());
  }
}

TEST_CASE("recent file dialog directories survive a save and restore",
          "[ui][settings]") {
  using ui::FileDialogMemory;

  QMap<QString, QString> dirs;
  dirs.insert("import-modlist", "/mods/dl");
  dirs.insert("export-modlist", "/mods/out");
  dirs.insert("settings-game-dir", "/games/skyrim");
  Settings::instance().set_recent_dirs(dirs);

  // A fresh app run reads the store back through restore().
  FileDialogMemory::restore();
  REQUIRE(FileDialogMemory::start_dir("import-modlist", QString()) == "/mods/dl");
  REQUIRE(FileDialogMemory::start_dir("export-modlist", QString()) == "/mods/out");
  REQUIRE(FileDialogMemory::start_dir("settings-game-dir", QString()) ==
          "/games/skyrim");
  // And it is what was stored, key for key - the parallel-list encoding must
  // not drop or transpose entries.
  REQUIRE(Settings::instance().recent_dirs() == dirs);
}