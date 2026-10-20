// All Mods > Enable/Disable must reach mods inside a FOLDED separator.
//
// The bulk toggle used to gate each row on the view's isRowHidden(), which
// conflates the text filter with separator folding: a mod inside a shut
// separator band is fold-hidden, so "Disable All" silently skipped it while
// the user believed the action covered the whole list. A mod the user
// believes is disabled but that is not can break a load order, so the silent
// partial application is the bug. MO2's setAllMatchingModsEnabled walks the
// MODEL instead (modlistviewactions.cpp:206-221 via flatIndex,
// modelutils.cpp:8-17), so folded separators never exclude a mod from the
// bulk toggle.
//
// Drives the REAL ModActions::toggle_all_mods against a REAL MainWindow with
// a REAL instance: one mod above the separator (stays visible), one inside
// the folded band (fold-hidden on the view). Disable All must disable BOTH;
// Enable All must re-enable both. The model row is the assertion surface:
// the gate that used to skip hidden rows sits immediately before the
// setData call, and the disk-sync callback is the same code for visible and
// hidden rows.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME / XDG_DATA_HOME,
// no network, no disk writes (the bare ModActions carries no sync callbacks).
#include "engine/game/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "ui/controllers/mod_actions.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char *kAlpha = "Alpha_mod";  // stays outside the separator
constexpr const char *kBeta  = "Beta_mod";   // ends up inside the folded band
constexpr const char *kRoot  = "/tmp/gmm_folded_bulk_toggle";

void write_file(const fs::path &p, const std::string &content) {
  std::ofstream out(p);
  out << content;
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

int row_of_id(const ui::ModList *model, const QString &id) {
  for (int r = 0; r < model->rowCount(); ++r) {
    if (model->mods().at(r).id == id)
      return r;
  }
  return -1;
}

}  // namespace

TEST_CASE("bulk enable/disable reaches mods inside a folded separator",
          "[ui][modlist]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = kRoot;
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "folded_separator_bulk_toggle_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  // set_game_info posts a singleShot(0) that opens a modal NXM-handler box -
  // an infinite hang offscreen.
  Settings::instance().set_nxm_handler_check("dont_ask");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const auto instance      = engine::Instance::from_root(inst_root);
  const fs::path mods_dir  = instance.path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / kAlpha);
  write_file(mods_dir / kAlpha / "meta.ini", "[General]\npriority=0\n");
  fs::create_directories(mods_dir / kBeta);
  write_file(mods_dir / kBeta / "meta.ini", "[General]\npriority=1\n");

  // The Default profile lists both mods enabled: without a modlist the model
  // loads every mod as disabled and the toggle below would be a no-op.
  const fs::path profiles = instance.path_for(engine::InstanceKind::Profiles);
  fs::create_directories(profiles / "Default");
  write_file(profiles / "Default" / "modlist.txt",
             std::string("+") + kAlpha + "\n+" + kBeta + "\n");

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
  auto *ctl   = w.findChild<ui::ModListController *>();
  auto *view  = w.mod_view();
  REQUIRE(model != nullptr);
  REQUIRE(ctl != nullptr);
  REQUIRE(view != nullptr);
  REQUIRE(pump_until([&] {
    return row_of_id(model, kBeta) >= 0;
  }));

  // A separator between the two mods: Alpha above it (stays visible), Beta
  // below it (inside the band).
  const QString sep_id = ctl->create_separator_named(QStringLiteral("Pack"), QString());
  REQUIRE_FALSE(sep_id.isEmpty());
  model->move_mod(sep_id, 1);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  const int alpha_row = row_of_id(model, kAlpha);
  const int sep_row   = row_of_id(model, sep_id);
  const int beta_row  = row_of_id(model, kBeta);
  REQUIRE(alpha_row >= 0);
  REQUIRE(sep_row > alpha_row);
  REQUIRE(beta_row > sep_row);
  REQUIRE(model->mods()[sep_row].is_separator);
  REQUIRE(model->has_content(sep_row));

  // Fold the band shut: Beta is now fold-hidden on the view, Alpha is not.
  // These preconditions are what make this the reported scenario - without
  // them a pass would mean nothing.
  model->set_folded(sep_row, true);
  model->apply_fold_state();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  REQUIRE(model->is_row_fold_hidden(beta_row));
  REQUIRE(view->isRowHidden(beta_row, QModelIndex()));
  REQUIRE_FALSE(model->is_row_fold_hidden(alpha_row));
  REQUIRE_FALSE(view->isRowHidden(alpha_row, QModelIndex()));

  // Both mods start enabled, so the toggle below is a real state change.
  REQUIRE(model->mods()[alpha_row].enabled);
  REQUIRE(model->mods()[beta_row].enabled);

  // ---- All Mods > Disable All ----------------------------------------
  ui::ModActions actions(&w);
  actions.toggle_all_mods(false);

  // THE POINT: the fold-hidden mod was disabled, not silently skipped.
  CHECK_FALSE(model->mods()[beta_row].enabled);
  // The visible mod is still reached.
  CHECK_FALSE(model->mods()[alpha_row].enabled);

  // ---- All Mods > Enable All -----------------------------------------
  actions.toggle_all_mods(true);
  CHECK(model->mods()[alpha_row].enabled);
  CHECK(model->mods()[beta_row].enabled);
}
