// Regression: "Change Categories" must write NOTHING when the submenu is
// opened and left alone.
//
// The commit-on-aboutToHide handler used to rebuild the id list by iterating
// the menu's categories, which are alphabetized by NAME, while the stored CSV
// is PRIMARY-FIRST. For a mod whose primary is not alphabetically first (Zebra
// id 5 before Apple id 3, CSV "5,3") the rebuild produced [3,5], which differs
// from `current`, so `apply` fired and `meta.save()` rewrote the CSV as "3,5" -
// silently flipping the primary and changing the Category column and the
// category filter from a menu that was only looked at.
//
// This drives the REAL ModContextMenu::add_category_menus against a real
// MainWindow and a real ModList model, opens the real submenu, closes it
// without touching a checkbox, and asserts the meta.ini on disk is
// byte-identical and the model still holds the primary-first order.
//
// The second case ticks a category and asserts the commit DOES happen, so the
// first case passing cannot be an artefact of a menu that never commits.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME / XDG_DATA_HOME.
#include "engine/game/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/plugin_host/category_factory.h"
#include "ui/controllers/mod_actions.h"
#include "ui/controllers/mod_context_menu.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMenu>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The two-category fixture, deliberately named so ALPHABETICAL order (Apple,
// Zebra) differs from ASCENDING-ID order (3, 5) and from the stored
// primary-first order (5, 3). Any rebuild-from-menu-order regression shows up
// here as a reordered CSV.
constexpr int kAppleId            = 3;
constexpr int kZebraId            = 5;
constexpr const char *kModId      = "Foo_mod";
constexpr const char *kPrimaryCsv = "5,3";

void write_file(const fs::path &p, const std::string &content) {
  std::ofstream out(p);
  out << content;
}

std::string read_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool pump_until(const std::function<bool()> &pred, int timeout_ms = 15000) {
  QElapsedTimer timer;
  timer.start();
  while (!pred()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(2);
    if (timer.elapsed() > timeout_ms)
      return false;
  }
  return true;
}

int find_mod_row(const ui::ModList *model, const QString &id) {
  for (int r = 0; r < model->rowCount(); ++r) {
    if (model->index(r, ui::ModList::Name).data().toString() == id)
      return r;
  }
  return -1;
}

// Replaces the global category registry with exactly the two fixture
// categories. MainWindow's settings controller seeds a core set during
// construction, so the clear has to happen after the window exists.
void seed_categories() {
  auto &factory = engine::Category::Factory::instance();
  for (const auto &[id, entry] : factory.categories()) {
    Q_UNUSED(entry);
    factory.removeCategory(id);
  }
  factory.addCategory(kAppleId, "Apple", 0);
  factory.addCategory(kZebraId, "Zebra", 0);
}

}  // namespace

TEST_CASE("Change Categories: open and leave writes nothing", "[ui][categories]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = "/tmp/gmm_change_categories_noop";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "change_categories_noop_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // set_game_info posts a singleShot(0) that opens a modal NXM-handler box -
  // an infinite hang offscreen. "dont_ask" is the early-out a user picks.
  Settings::instance().set_nxm_handler_check("dont_ask");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / kModId);
  const fs::path meta_ini = mods_dir / kModId / "meta.ini";
  write_file(meta_ini,
             std::string("[General]\npriority=0\ncategory=") + kPrimaryCsv + "\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(model, kModId) >= 0;
  }));

  seed_categories();

  // The mod scan migrates meta.ini into [GameModManager] on load, so the
  // baseline is captured AFTER the window is up, not from what we seeded.
  // The CSV itself is carried across unchanged - that is what we assert.
  const std::string before_bytes = read_file(meta_ini);
  REQUIRE(engine::ModMeta::load(mods_dir, kModId).get("General", "category") ==
          kPrimaryCsv);

  // The real menu builder, wired to the real window and model.
  ui::ModActions actions(&w);
  ui::ModContextMenu menu_builder(&w, &actions);
  QMenu menu;
  menu_builder.add_category_menus(menu, QLatin1String(kModId));

  QMenu *change_menu = nullptr;
  for (QAction *a : menu.actions()) {
    if (a->text() == QLatin1String("Change Categories")) {
      change_menu = qobject_cast<QMenu *>(a->menu());
      break;
    }
  }
  REQUIRE(change_menu != nullptr);

  // Both categories start checked, straight from the CSV - the check states
  // are the edit, so they must already agree with `current` before the user
  // touches anything.
  auto action_named = [&change_menu](const QString &name) {
    for (QAction *a : change_menu->actions()) {
      if (a->text() == name)
        return a;
    }
    return static_cast<QAction *>(nullptr);
  };
  QAction *apple = action_named(QLatin1String("Apple"));
  QAction *zebra = action_named(QLatin1String("Zebra"));
  REQUIRE(apple != nullptr);
  REQUIRE(zebra != nullptr);
  CHECK(apple->isChecked());
  CHECK(zebra->isChecked());

  const int row = find_mod_row(model, kModId);
  REQUIRE(row >= 0);
  const QVector<int> ids_before = model->mods().at(row).category_ids;
  const QString category_before = model->mods().at(row).category;
  REQUIRE(ids_before == QVector<int>{kZebraId, kAppleId});

  // Open the submenu and leave it. Nothing is toggled.
  change_menu->show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  change_menu->close();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // The file must be untouched - byte for byte, so even a rewrite that
  // happened to produce the same value would fail here.
  CHECK(read_file(meta_ini) == before_bytes);

  const auto csv = engine::ModMeta::load(mods_dir, kModId).get("General", "category");
  CHECK(csv == kPrimaryCsv);

  // And the model row is unchanged, so the Category column and the category
  // filter keep the primary-first order. The claim is the DELTA is empty; the
  // scan does not populate the display name, so it is compared, not asserted.
  const int row_after = find_mod_row(model, kModId);
  REQUIRE(row_after >= 0);
  CHECK(model->mods().at(row_after).category_ids == ids_before);
  CHECK(model->mods().at(row_after).category == category_before);
}

TEST_CASE("Change Categories: a tick still commits on hide", "[ui][categories]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = "/tmp/gmm_change_categories_commit";
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "change_categories_commit_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  Settings::instance().set_nxm_handler_check("dont_ask");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / kModId);
  const fs::path meta_ini = mods_dir / kModId / "meta.ini";
  write_file(meta_ini,
             std::string("[General]\npriority=0\ncategory=") + kPrimaryCsv + "\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *model = w.findChild<ui::ModList *>();
  REQUIRE(model != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(model, kModId) >= 0;
  }));

  seed_categories();

  ui::ModActions actions(&w);
  ui::ModContextMenu menu_builder(&w, &actions);
  QMenu menu;
  menu_builder.add_category_menus(menu, QLatin1String(kModId));

  QMenu *change_menu = nullptr;
  for (QAction *a : menu.actions()) {
    if (a->text() == QLatin1String("Change Categories")) {
      change_menu = qobject_cast<QMenu *>(a->menu());
      break;
    }
  }
  REQUIRE(change_menu != nullptr);

  // Un-tick Apple. The remaining primary stays Zebra, so the commit must
  // write "5" - not "3" and not the untouched "5,3".
  QAction *apple = nullptr;
  for (QAction *a : change_menu->actions()) {
    if (a->text() == QLatin1String("Apple"))
      apple = a;
  }
  REQUIRE(apple != nullptr);
  apple->setChecked(false);

  const std::string before_bytes = read_file(meta_ini);

  change_menu->show();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  change_menu->close();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  const auto csv = engine::ModMeta::load(mods_dir, kModId).get("General", "category");
  CHECK(csv == "5");
  // A real edit really did reach the disk - the no-op case is not passing
  // because this menu never writes.
  CHECK(read_file(meta_ini) != before_bytes);

  const int row = find_mod_row(model, kModId);
  REQUIRE(row >= 0);
  CHECK(model->mods().at(row).category_ids == QVector<int>{kZebraId});
  CHECK(model->mods().at(row).category.toStdString() == "Zebra");
}