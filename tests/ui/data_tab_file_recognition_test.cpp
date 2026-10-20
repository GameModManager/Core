// Data tab file recognition (size + open hand-off).
//
// The conflict registry's keys are CI-normalized (PathResolver::normalize
// lowercases every component, the filename included), but the row builder's
// disk lookup only matched directory components case-insensitively - the
// final filename was compared exactly. On a case-sensitive filesystem every
// mod file whose name carries an uppercase letter therefore resolved to
// nothing: the row appeared (the registry listed it) but showed no Size, and
// Open/Preview silently did nothing because the row had no real path. Only
// all-lowercase filenames survived - exactly the split in the user report
// (two lowercase-named .png rows with sizes, ~15 mixed-case ones without).
//
//   - every real file row carries a size after listing, whatever its name's
//     casing
//   - the open action hands a .txt to the OS handler (GMM's side: the row
//     resolves and open_requested fires with the on-disk path; the platform
//     call itself is a thin shell the OS owns)
//
// Hermetic: offscreen platform, throwaway /tmp/opencode tree, tiny registry
// (the background build + chunked apply are pumped on the main thread like
// data_tab_reorder_test).
#include "ui/panels/data_tab.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Registry =
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;

void write_file(const std::filesystem::path &path, const char *content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

// Pump the main thread until cond() holds or the deadline expires. The data
// tab builds rows on its worker thread and chunks them into the tree via
// queued signals / singleShot timers, so the test must spin the event loop.
template <typename Pred>
bool wait_for(Pred cond, int timeout_ms = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(5);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  return true;
}

QTreeWidgetItem *find_row(QTreeWidgetItem *parent, const QString &name) {
  for (int i = 0; i < parent->childCount(); ++i) {
    auto *child = parent->child(i);
    if (child->text(0) == name)
      return child;
    if (auto *found = find_row(child, name))
      return found;
  }
  return nullptr;
}

QTreeWidgetItem *find_row(QTreeWidget *tree, const QString &name) {
  return find_row(tree->invisibleRootItem(), name);
}

// The report's folder shape: most files are .png with mixed-case names, one
// is a .txt. Registry keys are the CI-normalized (all-lowercase) spellings
// the ConflictEngine produces on a case-sensitive filesystem.
struct Fixture {
  std::filesystem::path base;
  std::filesystem::path mods;
  Registry registry;
  QVector<ui::ModEntry> all_mods;
};

Fixture make_fixture(const std::string &name) {
  Fixture f;
  f.base = "/tmp/opencode/" + name;
  std::filesystem::remove_all(f.base);
  f.mods = f.base / "mods";
  // Mixed-case names: the class the report is about.
  write_file(f.mods / "ModA" / "content" / "gfx" / "Soapy Keeper.png", "x");
  write_file(f.mods / "ModA" / "content" / "gfx" / "Webbed Keeper Final.PNG", "xx");
  write_file(f.mods / "ModA" / "content" / "gfx" / "Rip Minisaac Sign.png", "xxx");
  // All-lowercase: the class that already worked.
  write_file(f.mods / "ModA" / "content" / "gfx" / "plain note.txt", "txt");
  // Mixed-case .txt: the file the user could not open.
  write_file(f.mods / "ModA" / "content" / "ReadMe.txt", "readme");

  ui::ModEntry mod_a;
  mod_a.id       = "ModA";
  mod_a.name     = "Mod A";
  mod_a.priority = 1;
  f.all_mods     = {mod_a};

  f.registry = {{"content/gfx/soapy keeper.png", {{"ModA", 1}}},
                {"content/gfx/webbed keeper final.png", {{"ModA", 1}}},
                {"content/gfx/rip minisaac sign.png", {{"ModA", 1}}},
                {"content/gfx/plain note.txt", {{"ModA", 1}}},
                {"content/readme.txt", {{"ModA", 1}}}};
  return f;
}

// Protected double-click internals, driven directly (open_item is protected
// so tests can run the open path without a synthetic click event).
struct TestableDataTab : ui::DataTab {
  using ui::DataTab::open_item;
};

}  // namespace

TEST_CASE("data tab rows carry a size whatever the file name's casing", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  const auto fx = make_fixture("gmm_data_tab_recognition_size");

  ui::DataTab tab;
  auto *tree = tab.tree();
  REQUIRE(tree);

  tab.show_data(fx.registry, fx.all_mods, false, fx.mods, {}, {}, "", "Data", false);
  // Rows display under the registry key's (CI-normalized) spelling.
  REQUIRE(wait_for([&] {
    return find_row(tree, "soapy keeper.png") != nullptr &&
           find_row(tree, "webbed keeper final.png") != nullptr &&
           find_row(tree, "rip minisaac sign.png") != nullptr &&
           find_row(tree, "plain note.txt") != nullptr &&
           find_row(tree, "readme.txt") != nullptr;
  }));

  // Every listed file is a real file on disk: every row shows a size. A row
  // with an empty Size column is the report's "does not recognize it as an
  // actual file" symptom.
  for (const QString &name :
       {QStringLiteral("soapy keeper.png"), QStringLiteral("webbed keeper final.png"),
        QStringLiteral("rip minisaac sign.png"), QStringLiteral("plain note.txt"),
        QStringLiteral("readme.txt")}) {
    auto *row = find_row(tree, name);
    REQUIRE(row);
    INFO("row without a size: " << name.toStdString());
    CHECK(!row->text(1).isEmpty());
  }

  std::filesystem::remove_all(fx.base);
}

TEST_CASE("data tab open hands a .txt to the OS handler", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  const auto fx = make_fixture("gmm_data_tab_recognition_open");

  TestableDataTab tab;
  auto *tree = tab.tree();
  REQUIRE(tree);

  QString opened;
  QObject::connect(&tab, &ui::DataTab::open_requested, &tab, [&](const QString &path) {
    opened = path;
  });

  tab.show_data(fx.registry, fx.all_mods, false, fx.mods, {}, {}, "", "Data", false);
  REQUIRE(wait_for([&] {
    return find_row(tree, "readme.txt") != nullptr;
  }));

  // GMM's side of the handoff: the .txt row resolves to its on-disk file and
  // the open action fires with that path - no extension gate decides it is
  // not openable. (The platform call itself is the OS's shell.)
  tab.open_item(find_row(tree, "readme.txt"));
  CHECK(!opened.isEmpty());
  CHECK(opened.endsWith("ReadMe.txt"));
  CHECK(QFileInfo(opened).exists());

  std::filesystem::remove_all(fx.base);
}
