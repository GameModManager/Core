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
//
// The rest pin WHERE the sentinel lands, which is the half a "does the file
// exist" assertion cannot see: a mod that lives in the game's mods dir must be
// written there even when the instance keeps a stub of the same folder and even
// when the row's display name is not its folder name.

#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "platform/platform.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QApplication>
#include <QDir>
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

// The view has no proxy (ModView::setModel installs the ModList directly), so
// a model row is the ModEntry index. Looking the row up by entry id - the
// folder name - is the only lookup that survives a display name which differs
// from it, which is exactly the case these toggles have to get right.
int find_mod_row_by_id(const ui::ModList *m, const QString &id) {
  const auto &entries = m->mods();
  for (int r = 0; r < static_cast<int>(entries.size()); ++r)
    if (entries[static_cast<size_t>(r)].id == id)
      return r;
  return -1;
}

// A throwaway Isaac world: the real plugin, a real instance, a real game dir.
// Declared here but constructed per-case, after the QApplication exists.
// setup_dirs() runs first so a case can lay the mod folders down before the
// window scans; open_window() only shows it.
struct IsaacWorld {
  explicit IsaacWorld(const char *tmp_name) : case_root(tmp_name) {}
  CaseRoot case_root;
  engine::PluginLoader loader;
  FakePlatform platform{case_root.root / "data"};
  ui::MainWindow w;
  fs::path inst_root;
  fs::path mods_dir;
  fs::path game_dir;

  void setup_dirs() {
    auto inst = engine::Instance::installed(kGameId, case_root.root / "instances");
    inst.info().game_id = kGameId;
    REQUIRE(inst.create_directories());
    REQUIRE(inst.write_toml());
    inst_root = inst.info().root;
    mods_dir =
        engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
    game_dir = case_root.root / "game";
    fs::create_directories(game_dir);
  }
  void open_window() {
    REQUIRE(loader.load_plugin(GMM_ISAAC_PLUGIN_PATH));
    auto &knowledge = loader.knowledge();
    w.set_game_knowledge(&knowledge);
    w.set_platform(&platform);
    w.show();
    w.set_game_info(kGameId, "The Binding of Isaac: Rebirth", "Default", game_dir,
                    inst_root);
    REQUIRE(pump_until([this] {
      return !w.is_loading();
    }));
  }
  [[nodiscard]] ui::ModList *model() const {
    return qobject_cast<ui::ModList *>(w.mod_view()->model());
  }
};

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
//
// The mod lives in the INSTANCE mods dir and has no counterpart in the game's
// own mods dir, i.e. the instance IS the mod. The sentinel belongs there, and
// nowhere else - the game-dir path must stay untouched.
TEST_CASE("Isaac: ticking a mod off writes disable.it immediately",
          "[ui][isaac][disable_it]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "disable_it_toggle_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  IsaacWorld world("gmm_disable_it_toggle");
  world.setup_dirs();

  fs::create_directories(world.mods_dir / "SampleMod");
  write_file(world.mods_dir / "SampleMod" / "meta.ini",
             "[General]\nname=Sample Mod\nversion=1.0\npriority=0\n");

  world.open_window();

  auto *model = world.model();
  REQUIRE(model != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row_by_id(model, QStringLiteral("SampleMod")) >= 0;
  }));

  const int row           = find_mod_row_by_id(model, QStringLiteral("SampleMod"));
  const fs::path sentinel = world.mods_dir / "SampleMod" / kSentinel;
  REQUIRE_FALSE(fs::exists(sentinel));

  // Off: the sentinel must exist the instant the tick is applied - no launch,
  // no flush, no waiting.
  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Unchecked),
                 Qt::CheckStateRole);
  CHECK(fs::exists(sentinel));
  CHECK_FALSE(fs::exists(world.game_dir / "mods" / "SampleMod" / kSentinel));

  // On: and it must be removed again, or the mod is stuck off.
  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Checked),
                 Qt::CheckStateRole);
  CHECK_FALSE(fs::exists(sentinel));
}

// The reported bug: a mod whose display name is not its folder name (an Isaac
// workshop mod, titled by metadata.xml) got its toggle resolved against the
// display name, so no candidate folder existed and the sentinel went nowhere.
// Asserting the exact path is the whole point - a bare "some disable.it exists"
// would pass against the instance stub and hide the defect again.
TEST_CASE("Isaac: an external mod's sentinel lands in the game mods dir",
          "[ui][isaac][disable_it]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "disable_it_toggle_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  IsaacWorld world("gmm_disable_it_external");
  world.setup_dirs();

  // Real content in the game's mods dir; the instance keeps a meta.ini-only
  // stub of the same folder, exactly as the scan worker leaves a workshop mod.
  const QString folder    = QStringLiteral("fiendfolio-reloaded_3778123093");
  const QString title     = QStringLiteral("Damn, Fiend Folio: RELOADED");
  const fs::path game_mod = world.game_dir / "mods" / folder.toStdString();
  write_file(game_mod / "metadata.xml", "<metadata><name>" + title.toStdString() +
                                            "</name><version>2.1</version>"
                                            "</metadata>");
  write_file(world.mods_dir / folder.toStdString() / "meta.ini",
             "[General]\nname=" + title.toStdString() + "\nversion=2.1\n");

  world.open_window();

  auto *model = world.model();
  REQUIRE(model != nullptr);
  REQUIRE(pump_until([&] {
    return find_mod_row_by_id(model, folder) >= 0;
  }));

  const int row = find_mod_row_by_id(model, folder);
  // The row is found by folder name, so the display name is free to differ -
  // that difference is what the toggle has to survive.
  CHECK(model->mods()[static_cast<size_t>(row)].name == title);
  // The external source is recorded on the row, so resolution never has to
  // guess between the instance stub and the game dir.
  CHECK(QDir::toNativeSeparators(model->mods()[static_cast<size_t>(row)].content_dir) ==
        QDir::toNativeSeparators(QString::fromStdString(game_mod.string())));

  const fs::path game_sentinel     = game_mod / kSentinel;
  const fs::path instance_sentinel = world.mods_dir / folder.toStdString() / kSentinel;
  REQUIRE_FALSE(fs::exists(game_sentinel));

  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Unchecked),
                 Qt::CheckStateRole);
  CHECK(fs::exists(game_sentinel));
  CHECK_FALSE(fs::exists(instance_sentinel));

  // Source gone: the row's recorded content_dir now points at a folder that is
  // not there, so resolution must fall through to the instance folder rather
  // than write into the missing one.
  std::error_code ec;
  fs::remove_all(game_mod, ec);
  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Checked),
                 Qt::CheckStateRole);
  CHECK_FALSE(fs::exists(game_sentinel));
  model->setData(model->index(row, ui::ModList::Name), QVariant(Qt::Unchecked),
                 Qt::CheckStateRole);
  CHECK(fs::exists(instance_sentinel));
  CHECK_FALSE(fs::exists(game_sentinel));
}
