// Isaac's disable.it sentinel: ticking a mod's checkbox writes it at once.
//
// The sentinel is the only on-disk record that a mod is off, so a toggle that
// never reaches the mod folder leaves the row reading "disabled" while the mod
// stays live. This drives the real MainWindow against the REAL Isaac game
// plugin's knowledge rather than a hand-written GameKnowledge: a hand-written
// store would pass even if the plugin still asked for the deferred path, which
// is exactly the case this pins down.
//
// The second case is the cheap pin on the cause - the plugin must not declare
// delayed_disable, because that declaration is what routes the toggle away from
// the write.

#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "platform/platform.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QVariant>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *kGameId = "TheBindingOfIsaacRebirth";
constexpr const char *kSentinel = "disable.it";

#ifndef GMM_ISAAC_PLUGIN_PATH
#define GMM_ISAAC_PLUGIN_PATH "TheBindingOfIsaacRebirth.so"
#endif

class FakePlatform : public engine::Platform {
public:
  explicit FakePlatform(fs::path data_dir) : data_dir_(std::move(data_dir)) {}
  std::string platform_name() const override { return "fake"; }
  fs::path data_dir() const override { return data_dir_; }
  fs::path config_dir() const override { return data_dir_; }
  fs::path cache_dir() const override { return data_dir_; }
  fs::path home_dir() const override { return data_dir_; }
  fs::path temp_dir() const override { return data_dir_; }
  fs::path find_steam_root() const override { return {}; }
  bool launch_executable(const fs::path &,
                         const std::vector<std::string> &) const override {
    return false;
  }

private:
  fs::path data_dir_;
};

// Per-case throwaway root, removed on scope exit even when an assertion FAILs,
// so a red run leaves nothing behind. XDG_CONFIG_HOME / XDG_DATA_HOME point
// into it, so Settings and the freedesktop trash stay out of the real ones.
struct CaseRoot {
  explicit CaseRoot(const char *name) : root(fs::path("/tmp") / name) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "config");
    fs::create_directories(root / "data");
    fs::create_directories(root / "instances");
    qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
    qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
    // set_game_info posts ensure_nxm_handler_default() via singleShot(0), which
    // pops a modal QMessageBox inside the first processEvents - an endless
    // hang offscreen. This setting is its early-out.
    Settings::instance().set_nxm_handler_check("dont_ask");
  }
  ~CaseRoot() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  CaseRoot(const CaseRoot &)            = delete;
  CaseRoot &operator=(const CaseRoot &) = delete;

  fs::path root;
};

template <typename Fn> bool pump_until(Fn pred, int timeout_ms = 15000) {
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

void write_file(const fs::path &p, const std::string &content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << content;
}

// ModList column 0 is the fold indicator, not the name, so scan every cell of
// every row rather than assuming a column.
int find_mod_row(const QAbstractItemModel *m, const QString &id) {
  for (int r = 0; r < m->rowCount(); ++r)
    for (int c = 0; c < m->columnCount(); ++c)
      if (m->index(r, c).data().toString() == id)
        return r;
  return -1;
}

}  // namespace

// The plugin's declaration is the input that decides whether the toggle writes
// now or defers to launch. Pinned on its own so a failure here names the cause
// instead of leaving it to be inferred from the sentinel assertions.
TEST_CASE("Isaac: the game plugin asks for an immediate sentinel write",
          "[ui][isaac][disable_it]") {
  engine::PluginLoader loader;
  REQUIRE(loader.load_plugin(GMM_ISAAC_PLUGIN_PATH));

  const auto &knowledge = loader.knowledge();
  REQUIRE(knowledge.has(kGameId, "disable_mechanism"));
  CHECK(knowledge.get(kGameId, "disable_mechanism") == kSentinel);
  CHECK_FALSE(engine::delayed_disable_for(knowledge, kGameId));
}

// End to end through the real window: tick the checkbox, assert the sentinel is
// already in the mod folder, tick back, assert it is gone.
TEST_CASE("Isaac: ticking a mod off writes disable.it immediately",
          "[ui][isaac][disable_it]") {
  CaseRoot case_root("gmm_disable_it_toggle");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "disable_it_toggle_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // The real plugin, so the knowledge under test is the shipped declaration.
  engine::PluginLoader loader;
  REQUIRE(loader.load_plugin(GMM_ISAAC_PLUGIN_PATH));
  auto &knowledge = loader.knowledge();

  auto inst            = engine::Instance::installed(kGameId, case_root.root / "instances");
  inst.info().game_id  = kGameId;
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "SampleMod");
  write_file(mods_dir / "SampleMod" / "meta.ini",
             "[General]\nname=Sample Mod\nversion=1.0\npriority=0\n");

  // sync_mod_enable_state declines to touch disk without a game dir, so the
  // case needs a real one - the toggle being proven is not the no-op branch.
  const fs::path game_dir = case_root.root / "game";
  fs::create_directories(game_dir);

  FakePlatform platform(case_root.root / "data");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info(kGameId, "The Binding of Isaac: Rebirth", "Default", game_dir,
                  inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  auto *view = w.mod_view();
  REQUIRE(view != nullptr);
  auto *model = view->model();
  REQUIRE(model != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row(model, QStringLiteral("SampleMod")) >= 0;
  }));

  const int row            = find_mod_row(model, QStringLiteral("SampleMod"));
  const fs::path sentinel  = mods_dir / "SampleMod" / kSentinel;
  REQUIRE_FALSE(fs::exists(sentinel));

  // Off: the sentinel must exist the instant the tick is applied - no launch,
  // no flush, no waiting.
  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Unchecked),
                 Qt::CheckStateRole);
  CHECK(fs::exists(sentinel));

  // On: and it must be removed again, or the mod is stuck off.
  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Checked),
                 Qt::CheckStateRole);
  CHECK_FALSE(fs::exists(sentinel));
}
