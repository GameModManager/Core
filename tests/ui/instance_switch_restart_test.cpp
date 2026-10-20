// The restart-only instance switch (Workspace-gb0b phase 2, the wiring).
//
// Settings > General > "Restart when switching game instance" (default on)
// makes every switch go through the plan -> Restart -> relaunch with
// --instance <target>. The relaunch itself is never executed here: the test
// injects a restart seam (SettingsController::set_restart_hook) and drives
// the REAL switch_to_instance(), asserting that it asked for the relaunch,
// persisted the last instance, and did NOT live-mutate the running window -
// the whole point of the setting. With the setting off, the same switch
// takes the live path (the negative control: no relaunch, the window really
// becomes the target).
//
// The two fixture instances bind DIFFERENT games on purpose: current_game_id()
// is the observable that tells a live switch (it changes) from a restart
// (it cannot - nothing ran).
//
// Modeled on data_tab_instance_switch_test.cpp: real MainWindow, offscreen
// platform, throwaway /tmp root (HOME + XDG_CONFIG_HOME + XDG_DATA_HOME),
// no network, no real game install.
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "platform/platform.h"
#include "test_platform.h"
#include "ui/controllers/settings_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/task_dialog.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path &p, const char *content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

// Pump the main thread until cond() holds or the deadline expires. The mod
// scan and the rest of the set_game_info chain run on worker threads and
// land via queued signals, so the test must spin the event loop.
template <typename Pred>
bool pump_until(Pred cond, int timeout_ms = 20000) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(5);
    if (timer.elapsed() > timeout_ms)
      return false;
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  return true;
}

// Every case starts from the same clean slate. The SAME path every time:
// Settings::instance() binds its QSettings file at first construction, so
// per-case roots would leak state across cases through the singleton.
fs::path make_case_root() {
  const fs::path root = fs::path("/tmp/gmm_switch_restart");
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  // default_instances_dir() = $HOME/.local/share/GameModManager/instances,
  // so the fixture instances must live under the redirected HOME.
  fs::create_directories(root / ".local/share/GameModManager/instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  qputenv("HOME", QByteArray(root.string().c_str()));
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0);
  // the "dont_ask" setting is its early-out (main_window_harness_test).
  Settings::instance().set_nxm_handler_check("dont_ask");
  return root;
}

// Platform stub with throwaway dirs, the main_window_harness_test shape.
class FakePlatform : public gmm_test::InertPlatform {
public:
  explicit FakePlatform(fs::path data_dir) : data_dir_(std::move(data_dir)) {}
  std::string platform_name() const override { return "fake"; }
  fs::path data_dir() const override { return data_dir_; }
  fs::path config_dir() const override { return data_dir_; }
  fs::path cache_dir() const override { return data_dir_; }
  fs::path home_dir() const override { return data_dir_; }
  fs::path temp_dir() const override { return data_dir_; }

private:
  fs::path data_dir_;
};

// One instance bound to one game, with one mod, so the live path (the
// negative control) has a real set_game_info to run.
struct InstanceFixture {
  fs::path game_dir;
  fs::path instance_root;
};

InstanceFixture make_instance(const fs::path &instances_dir, const std::string &name,
                              const std::string &game_id) {
  InstanceFixture f;
  f.game_dir = instances_dir.parent_path() / (name + "_game");
  write_file(f.game_dir / "Data" / "Skyrim.esm", "fake esm");

  auto inst           = engine::Instance::installed(name, instances_dir);
  inst.info().game_id = game_id;
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  f.instance_root = inst.info().root;

  const auto inst_view    = engine::Instance::from_root(f.instance_root);
  const fs::path mods_dir = inst_view.path_for(engine::InstanceKind::Mods);
  write_file(mods_dir / "Mod" / "meta.ini", "[General]\npriority=0\n");
  write_file(mods_dir / "Mod" / "a_file.txt", "content");
  const fs::path profiles_dir = inst_view.path_for(engine::InstanceKind::Profiles);
  write_file(profiles_dir / "Default" / "modlist.txt", "+Mod\n");
  return f;
}

// Click a command link by text in whatever modal dialog is up (the
// instance_restart_dialog_test pattern, adapted for a dialog this test does
// not construct itself). Falls back to closing it.
void click_modal_link(const QString &text) {
  QTimer::singleShot(0, [text] {
    auto *modal = QApplication::activeModalWidget();
    if (!modal)
      return;
    for (auto *b : modal->findChildren<QCommandLinkButton *>()) {
      if (b->text() == text) {
        b->click();
        return;
      }
    }
    modal->close();
  });
}

}  // namespace

TEST_CASE("a switch with restart-on-switch on relaunches into the target", "[ui]") {
  const fs::path root      = make_case_root();
  const auto instances_dir = root / ".local/share/GameModManager/instances";
  const auto a             = make_instance(instances_dir, "instA", "testgame");
  const auto b             = make_instance(instances_dir, "instB", "othertestgame");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "instance_switch_restart_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  app.setQuitOnLastWindowClosed(false);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("othertestgame", "mods_subpath", "Mods");

  FakePlatform platform(root / "data");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", a.game_dir, a.instance_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));
  REQUIRE(w.current_game_id() == "testgame");

  // Confirmation off: the plan is Restart with nothing to ask.
  Settings::instance().set_restart_on_instance_switch(true);
  Settings::instance().set_show_change_game_confirmation(false);

  std::optional<QStringList> relaunched;
  w.settings_controller()->set_restart_hook([&relaunched](const QStringList &args) {
    REQUIRE_FALSE(relaunched.has_value());  // exactly one relaunch
    relaunched = args;
  });

  CHECK(w.settings_controller()->switch_to_instance("instB"));

  // The relaunch was requested with the target instance key.
  REQUIRE(relaunched.has_value());
  CHECK(*relaunched == QStringList{"--instance", "instB"});

  // No live mutation: the running window never became instance B. This is
  // the property the setting exists for - the new process starts fresh.
  CHECK(w.current_game_id() == "testgame");

  // The relaunch target is persisted for the new process to pick up.
  CHECK(engine::read_last_instance() == "instB");
}

TEST_CASE("the restart confirmation offers restart or cancel, never continue", "[ui]") {
  const fs::path root      = make_case_root();
  const auto instances_dir = root / ".local/share/GameModManager/instances";
  const auto a             = make_instance(instances_dir, "instA", "testgame");
  const auto b             = make_instance(instances_dir, "instB", "othertestgame");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "instance_switch_restart_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  app.setQuitOnLastWindowClosed(false);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("othertestgame", "mods_subpath", "Mods");

  FakePlatform platform(root / "data");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", a.game_dir, a.instance_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  // Both defaults on: the plan is Ask, and with restart-on-switch on there
  // is no honest "continue without reset", so the dialog must not offer it.
  Settings::instance().set_restart_on_instance_switch(true);
  Settings::instance().set_show_change_game_confirmation(true);

  std::optional<QStringList> relaunched;
  w.settings_controller()->set_restart_hook([&relaunched](const QStringList &args) {
    REQUIRE_FALSE(relaunched.has_value());
    relaunched = args;
  });

  // Restart confirms: the relaunch is requested, the window is not
  // live-mutated.
  click_modal_link("Restart");
  CHECK(w.settings_controller()->switch_to_instance("instB"));
  REQUIRE(relaunched.has_value());
  CHECK(*relaunched == QStringList{"--instance", "instB"});
  CHECK(w.current_game_id() == "testgame");
  CHECK(engine::read_last_instance() == "instB");
}

TEST_CASE("cancelling the restart confirmation aborts the switch", "[ui]") {
  const fs::path root      = make_case_root();
  const auto instances_dir = root / ".local/share/GameModManager/instances";
  const auto a             = make_instance(instances_dir, "instA", "testgame");
  const auto b             = make_instance(instances_dir, "instB", "othertestgame");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "instance_switch_restart_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  app.setQuitOnLastWindowClosed(false);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("othertestgame", "mods_subpath", "Mods");

  FakePlatform platform(root / "data");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", a.game_dir, a.instance_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  Settings::instance().set_restart_on_instance_switch(true);
  Settings::instance().set_show_change_game_confirmation(true);

  std::optional<QStringList> relaunched;
  w.settings_controller()->set_restart_hook([&relaunched](const QStringList &args) {
    relaunched = args;
  });

  // Cancel (or closing the dialog): stay on the current instance, touch
  // nothing.
  click_modal_link("Cancel");
  CHECK_FALSE(w.settings_controller()->switch_to_instance("instB"));
  CHECK_FALSE(relaunched.has_value());
  CHECK(w.current_game_id() == "testgame");
  CHECK(engine::read_last_instance() != "instB");
}

TEST_CASE("with restart-on-switch off an identical switch stays live", "[ui]") {
  const fs::path root      = make_case_root();
  const auto instances_dir = root / ".local/share/GameModManager/instances";
  const auto a             = make_instance(instances_dir, "instA", "testgame");
  const auto b             = make_instance(instances_dir, "instB", "othertestgame");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "instance_switch_restart_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  app.setQuitOnLastWindowClosed(false);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("othertestgame", "mods_subpath", "Mods");

  FakePlatform platform(root / "data");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.set_platform(&platform);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", a.game_dir, a.instance_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  // The OFF fallback: today's rules. Neither instance declares appearance
  // overrides, so the plan is Live - the negative control for everything
  // the restart path asserts above.
  Settings::instance().set_restart_on_instance_switch(false);
  Settings::instance().set_show_change_game_confirmation(false);

  std::optional<QStringList> relaunched;
  w.settings_controller()->set_restart_hook([&relaunched](const QStringList &args) {
    relaunched = args;
  });

  CHECK(w.settings_controller()->switch_to_instance("instB"));

  // No relaunch was requested - the live path ran instead.
  CHECK_FALSE(relaunched.has_value());
  // The window really became instance B: the live mutation happened.
  CHECK(w.current_game_id() == "othertestgame");
  CHECK(engine::read_last_instance() == "instB");
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));
}
