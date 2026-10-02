// Full-window GUI coverage for the load-order / mod-list backup and restore
// points (Workspace-czc0).
//
// Drives the REAL ui::MainWindow offscreen with QTest clicks on the real
// toolbar buttons, then asserts the bytes on disk. The point of these cases is
// CONSUMER-REACHES-CONTROL, not a getter round-trip: the button click is the
// whole chain (toolbar -> PluginsTab/PluginView signal -> BackupActions ->
// engine::backup -> copy_file), and the only thing worth asserting is what the
// files look like afterwards.
//
// What is covered here and why it earns a test:
//   - the Backup button really writes the three stamped plugin files, byte for
//     byte, next to the live ones
//   - Restore CONFIRMS first (MO2 overwrites silently), and a refusal changes
//     nothing on disk
//   - a completed Restore really puts the bytes back AND leaves the pre-restore
//     state recoverable as a safety backup (MO2 has no such copy)
//   - a partial Restore names the file it did not restore, so the profile is
//     never left mixed in silence (MO2 ||-chains its three copies)
//   - a Restore whose safety copy failed for one file reports the mix, and
//     never claims nothing was overwritten while its siblings were replaced
//   - the picker with nothing to offer says so rather than failing quietly
//   - the mod-list pair behaves like the load-order pair
//
// Hermeticity: one throwaway root per TEST_CASE with XDG_CONFIG_HOME and
// XDG_DATA_HOME inside it, the main_window_harness_test.cpp shape.
#include "engine/core/instance/instance.h"
#include "engine/game/registry/game_capabilities.h"
#include "engine/game/registry/game_knowledge.h"
#include "platform/platform.h"
#include "ui/controllers/backup_actions.h"
#include "ui/controllers/mod_list_controller.h"
#include "ui/main_window/main_window.h"
#include "ui/panels/plugin_view.h"
#include "ui/panels/plugins_tab.h"
#include "ui/settings/settings.h"
#include "ui/widgets/error_popup.h"
#include "ui/widgets/right_panel.h"
#include "ui/widgets/task_dialog.h"

#include <QApplication>
#include <QCommandLinkButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path &p, const std::string &content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

std::string read_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

template <typename Fn>
bool pump_until(Fn pred, int timeout_ms = 20000) {
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

// Per-case throwaway root: config/data/instances live inside it, so Settings
// (XDG_CONFIG_HOME) and the freedesktop trash (XDG_DATA_HOME) are isolated per
// TEST_CASE and never shared with another test.
//
// The name must NOT start with "gmm": install_method_test."the probe creates
// nothing" asserts that nothing matching /tmp/gmm* appears while it runs, so a
// concurrently running case that creates one makes it fail.
fs::path make_case_root(const char *name) {
  const fs::path root = fs::path("/tmp") / name;
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));
  // set_game_info posts ensure_nxm_handler_default() via singleShot(0); it pops
  // a modal NXM-handler box inside the first processEvents (an infinite hang
  // offscreen). "dont_ask" is its early-out.
  Settings::instance().set_nxm_handler_check("dont_ask");
  return root;
}

void click_tab(QTabWidget *tabs, const QString &label) {
  REQUIRE(tabs != nullptr);
  auto *bar = tabs->findChild<QTabBar *>();
  REQUIRE(bar != nullptr);
  for (int i = 0; i < bar->count(); ++i) {
    if (bar->tabText(i) == label) {
      QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(i).center());
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      return;
    }
  }
  FAIL("no tab labelled " << label.toStdString());
}

// A button by object name, anywhere under `root`. The backup / restore buttons
// carry no text that is unique across the two tabs (both say "Backup"), so the
// object name is what identifies them.
QPushButton *find_button(QWidget *root, const char *name) {
  auto *button = root->findChild<QPushButton *>(QString::fromLatin1(name));
  REQUIRE(button != nullptr);
  return button;
}

// The stamp suffixes of `<dir>/<file_name>.*`, sorted.
std::vector<std::string> stamps_of(const fs::path &dir, const std::string &file_name) {
  std::vector<std::string> stamps;
  const std::string prefix = file_name + ".";
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().string();
    if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
      stamps.push_back(name.substr(prefix.size()));
  }
  std::sort(stamps.begin(), stamps.end());
  return stamps;
}

// The suffixes that are REAL stamps (the engine owns that judgement; this only
// needs "the one the Backup button just made").
std::vector<std::string> backup_stamps_of(const fs::path &dir,
                                          const std::string &file_name) {
  std::vector<std::string> out;
  for (const auto &suffix : stamps_of(dir, file_name)) {
    if (engine::backup::is_backup_stamp(suffix))
      out.push_back(suffix);
  }
  return out;
}

// Drives whatever modal appears next, whichever kind it is: the backup picker
// (pick the first row and press OK), a QMessageBox (record its text), or the
// confirmation TaskDialog (click the named command link). Armed BEFORE the
// click, because QDialog::exec() spins its own event loop - a poll is the only
// thing that gets to run inside it.
class ModalDriver {
public:
  void arm(const QString &command_link = QStringLiteral("Yes")) {
    link_ = command_link;
    seen_ = 0;
    infos_.clear();
    poll_.setInterval(20);
    QObject::connect(&poll_, &QTimer::timeout, [this] {
      tick();
    });
    poll_.start();
  }

  void disarm() { poll_.stop(); }

  // How many modals this driver answered. The restore path legitimately shows
  // two in a row (picker, then confirmation), so "answered at least one" is
  // not enough to say the scripted gesture reached the end.
  int answered() const { return seen_; }
  // Text of every QMessageBox that was shown, in order.
  const QStringList &infos() const { return infos_; }

private:
  void tick() {
    auto *modal = QApplication::activeModalWidget();
    // Every modal is answered, exactly once: a driver that only handled the
    // first would close the confirmation unanswered and silently cancel the
    // operation it is supposed to be testing.
    if (!modal || answered_.count(modal) != 0)
      return;
    answered_.insert(modal);
    ++seen_;
    {
      // The picker: a QDialog carrying a QListWidget and a button box.
      if (auto *list = modal->findChild<QListWidget *>()) {
        list->setCurrentRow(0);
        if (auto *box = modal->findChild<QDialogButtonBox *>()) {
          if (auto *ok = box->button(QDialogButtonBox::Ok)) {
            ok->click();
            return;
          }
        }
        modal->close();
        return;
      }
      // An information box: record what it said, then dismiss it.
      if (auto *box = qobject_cast<QMessageBox *>(modal)) {
        infos_ << box->text();
        box->accept();
        return;
      }
      // The confirmation: a command link with the requested text.
      for (auto *button : modal->findChildren<QCommandLinkButton *>()) {
        if (button->text() == link_) {
          button->click();
          return;
        }
      }
      modal->close();
      return;
    }
  }

  QTimer poll_;
  QString link_;
  int seen_ = 0;
  QStringList infos_;
  std::set<QWidget *> answered_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Golden path: the four buttons exist and are reachable, the load-order Backup
// writes three stamped copies next to the live files, and the picker with
// nothing to offer says so instead of failing quietly.
// ---------------------------------------------------------------------------
TEST_CASE("Backup button stamps the three plugin files; empty picker reports it",
          "[ui][harness][backup]") {
  const fs::path root = make_case_root("czc0_backup");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "backup_restore_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");
  write_file(game_dir / "Data" / "Update.esm", "fake esm");
  write_file(game_dir / "Data" / "MyMod.esp", "fake esp");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm,Update.esm");

  engine::GameCapabilities caps;
  engine::CapabilityInfo plugins_cap;
  plugins_cap.game_id      = "testgame";
  plugins_cap.capability   = "plugins";
  plugins_cap.display_name = "Plugins";
  caps.register_capability(plugins_cap);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  click_tab(rp->tab_widget(), QStringLiteral("Plugins"));
  auto *ptab = rp->plugins_tab();
  REQUIRE(ptab != nullptr);

  const fs::path profile_dir = engine::Instance::from_root(inst.info().root)
                                   .path_for(engine::InstanceKind::Profiles) /
                               "Default";
  REQUIRE(fs::is_directory(profile_dir));
  REQUIRE(fs::is_regular_file(profile_dir / "plugins.txt"));
  REQUIRE(fs::is_regular_file(profile_dir / "loadorder.txt"));
  REQUIRE(fs::is_regular_file(profile_dir / "lockedorder.txt"));

  // The mod-list pair is in the counter row of the mods tab, not the plugins
  // tab; both are present from construction.
  auto *mod_backup  = find_button(&w, "modBackupBtn");
  auto *mod_restore = find_button(&w, "modRestoreBtn");
  CHECK(mod_backup != nullptr);
  CHECK(mod_restore != nullptr);

  // Nothing backed up yet: the picker has nothing to offer and says so. MO2's
  // words (mainwindow.cpp:3885-3886) - a silent no-op here would read as a
  // broken button.
  CHECK(backup_stamps_of(profile_dir, "plugins.txt").empty());
  {
    ModalDriver driver;
    driver.arm();
    QTest::mouseClick(find_button(ptab, "pluginRestoreBtn"), Qt::LeftButton);
    REQUIRE(driver.answered() > 0);
    driver.disarm();
    REQUIRE(driver.infos().size() == 1);
    CHECK(driver.infos().first().contains("no backups to restore"));
  }
  // The refusal changed nothing.
  CHECK(backup_stamps_of(profile_dir, "plugins.txt").empty());
  CHECK(fs::is_regular_file(profile_dir / "plugins.txt"));

  // Now the Backup button, for real.
  QTest::mouseClick(find_button(ptab, "pluginBackupBtn"), Qt::LeftButton);
  REQUIRE(pump_until([&] {
    return !backup_stamps_of(profile_dir, "plugins.txt").empty();
  }));

  const auto stamps = backup_stamps_of(profile_dir, "plugins.txt");
  REQUIRE(stamps.size() == 1);
  const std::string stamp = stamps.front();

  // All three files of the load-order set carry the ONE stamp, and each copy is
  // byte-for-byte identical to the live file it was copied from.
  for (const char *name : {"plugins.txt", "loadorder.txt", "lockedorder.txt"}) {
    const fs::path backup = profile_dir / (std::string(name) + "." + stamp);
    INFO("backup: " << backup.string());
    REQUIRE(fs::is_regular_file(backup));
    CHECK(read_file(backup) == read_file(profile_dir / name));
  }

  // The live files are untouched: a backup writes siblings, never the file.
  CHECK(fs::is_regular_file(profile_dir / "plugins.txt"));

  // NOT backed up: archives.txt and settings.ini are out of scope by design.
  CHECK(backup_stamps_of(profile_dir, "archives.txt").empty());
  CHECK(backup_stamps_of(profile_dir, "settings.ini").empty());

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Restore confirms before it overwrites (MO2 does not), a refusal writes
// nothing, and an accepted restore puts the bytes back AND leaves the state it
// replaced recoverable.
// ---------------------------------------------------------------------------
TEST_CASE("Restore confirms, then restores and leaves a safety backup",
          "[ui][harness][backup]") {
  const fs::path root = make_case_root("czc0_restore");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "backup_restore_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");
  write_file(game_dir / "Data" / "MyMod.esp", "fake esp");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm");

  engine::GameCapabilities caps;
  engine::CapabilityInfo plugins_cap;
  plugins_cap.game_id      = "testgame";
  plugins_cap.capability   = "plugins";
  plugins_cap.display_name = "Plugins";
  caps.register_capability(plugins_cap);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  click_tab(rp->tab_widget(), QStringLiteral("Plugins"));
  auto *ptab = rp->plugins_tab();
  REQUIRE(ptab != nullptr);

  const fs::path profile_dir = engine::Instance::from_root(inst.info().root)
                                   .path_for(engine::InstanceKind::Profiles) /
                               "Default";
  REQUIRE(fs::is_directory(profile_dir));

  // 1. Back the load order up through the real button.
  QTest::mouseClick(find_button(ptab, "pluginBackupBtn"), Qt::LeftButton);
  REQUIRE(pump_until([&] {
    return backup_stamps_of(profile_dir, "plugins.txt").size() == 1;
  }));
  const std::string stamp = backup_stamps_of(profile_dir, "plugins.txt").front();
  const std::string backed_up_loadorder =
      read_file(profile_dir / ("loadorder.txt." + stamp));

  // 2. Change the live files to something a user would want back.
  const std::string clobbered = "clobbered\nstate\n";
  write_file(profile_dir / "loadorder.txt", clobbered);
  write_file(profile_dir / "plugins.txt", "clobbered\n");
  write_file(profile_dir / "lockedorder.txt", "clobbered|0\n");

  // 3. Restore, and REFUSE at the confirmation. Divergence 1: MO2 overwrites
  //    with no prompt at all (mainwindow.cpp:3904-3906).
  {
    ModalDriver driver;
    driver.arm(QStringLiteral("Cancel"));
    QTest::mouseClick(find_button(ptab, "pluginRestoreBtn"), Qt::LeftButton);
    // Two modals: the picker, then the confirmation we refuse.
    REQUIRE(driver.answered() == 2);
    driver.disarm();
  }
  REQUIRE(read_file(profile_dir / "loadorder.txt") == clobbered);
  // A refusal takes no safety backup either - nothing was at risk.
  CHECK(backup_stamps_of(profile_dir, "loadorder.txt").size() == 1);

  // 4. Restore again and ACCEPT. The picker takes the newest entry, which is
  //    the backup from step 1 (only one exists).
  {
    ModalDriver driver;
    driver.arm(QStringLiteral("Restore"));
    QTest::mouseClick(find_button(ptab, "pluginRestoreBtn"), Qt::LeftButton);
    // Two modals: the picker, then the confirmation we accept.
    REQUIRE(driver.answered() == 2);
    driver.disarm();
  }
  REQUIRE(pump_until([&] {
    return read_file(profile_dir / "loadorder.txt") == backed_up_loadorder;
  }));
  CHECK(read_file(profile_dir / "plugins.txt") != "clobbered\n");

  // Divergence 2: the state the restore replaced is still on disk, under its own
  // stamp. MO2 has no such copy, so a wrong restore there is unrecoverable.
  const auto safety = backup_stamps_of(profile_dir, "loadorder.txt");
  REQUIRE(safety.size() == 2);
  REQUIRE(safety.back() != stamp);
  CHECK(read_file(profile_dir / ("loadorder.txt." + safety.back())) == clobbered);

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Divergence 3 at the UI level: one backup missing from the set must not take
// the other two with it, and the report must NAME the file that was left stale.
// ---------------------------------------------------------------------------
TEST_CASE("a partial restore reports which file it did not restore",
          "[ui][harness][backup]") {
  const fs::path root = make_case_root("czc0_partial");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "backup_restore_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");
  write_file(game_dir / "Data" / "MyMod.esp", "fake esp");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm");

  engine::GameCapabilities caps;
  engine::CapabilityInfo plugins_cap;
  plugins_cap.game_id      = "testgame";
  plugins_cap.capability   = "plugins";
  plugins_cap.display_name = "Plugins";
  caps.register_capability(plugins_cap);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  click_tab(rp->tab_widget(), QStringLiteral("Plugins"));
  auto *ptab = rp->plugins_tab();
  REQUIRE(ptab != nullptr);

  const fs::path profile_dir = engine::Instance::from_root(inst.info().root)
                                   .path_for(engine::InstanceKind::Profiles) /
                               "Default";
  REQUIRE(fs::is_directory(profile_dir));

  QTest::mouseClick(find_button(ptab, "pluginBackupBtn"), Qt::LeftButton);
  REQUIRE(pump_until([&] {
    return backup_stamps_of(profile_dir, "plugins.txt").size() == 1;
  }));
  const std::string stamp = backup_stamps_of(profile_dir, "plugins.txt").front();

  // Take one member out of the set - the MO2 ||-chain trap: a missing
  // loadorder.txt used to skip the other two copies with no message at all.
  std::error_code ec;
  REQUIRE(fs::remove(profile_dir / ("loadorder.txt." + stamp), ec));
  REQUIRE(!ec);

  const std::string clobbered = "clobbered\n";
  write_file(profile_dir / "loadorder.txt", clobbered);
  write_file(profile_dir / "plugins.txt", clobbered);
  write_file(profile_dir / "lockedorder.txt", clobbered);

  // Capture the report instead of showing it: the per-file text is what matters,
  // and a modal error box cannot be asserted on without scraping every label.
  QString report_main;
  QString report_details;
  ui::set_error_presenter_for_tests([&](ui::TaskDialog &dlg) {
    for (const auto *label : dlg.findChildren<QLabel *>())
      report_main += label->text() + "\n";
    if (const auto *edit = dlg.findChild<QPlainTextEdit *>())
      report_details += edit->toPlainText();
  });
  struct RestorePresenter {
    ~RestorePresenter() { ui::set_error_presenter_for_tests({}); }
  } restore_presenter;

  {
    ModalDriver driver;
    driver.arm(QStringLiteral("Restore"));
    QTest::mouseClick(find_button(ptab, "pluginRestoreBtn"), Qt::LeftButton);
    REQUIRE(driver.answered() == 2);
    driver.disarm();
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // The two files that DID have a backup were restored...
  CHECK(read_file(profile_dir / "plugins.txt") != clobbered);
  CHECK(read_file(profile_dir / "lockedorder.txt") != clobbered);
  // ...and the one that did not was left alone. MO2's ||-chain would have
  // skipped the other two as well and said nothing.
  CHECK(read_file(profile_dir / "loadorder.txt") == clobbered);
  // The report names it, so the user knows which half of the profile is stale.
  INFO("report: " << report_main.toStdString() << report_details.toStdString());
  CHECK_FALSE(report_details.isEmpty());
  CHECK(report_details.contains("loadorder.txt"));
  CHECK_FALSE(report_details.contains("plugins.txt"));
  CHECK_FALSE(report_details.contains("lockedorder.txt"));
  CHECK(report_main.toLower().contains("partly restored"));

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The safety backup can fail for one file while the others restore cleanly. The
// report must then describe what actually happened - some files changed, one did
// not - and must NEVER claim that nothing was overwritten. A feature whose whole
// job is safety cannot tell the user their data is intact when it is not.
// ---------------------------------------------------------------------------
TEST_CASE("a failed safety copy never reports that nothing was overwritten",
          "[ui][harness][backup]") {
  const fs::path root = make_case_root("czc0_lie");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "backup_restore_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "Data");
  write_file(game_dir / "Data" / "Skyrim.esm", "fake esm");
  write_file(game_dir / "Data" / "MyMod.esp", "fake esp");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  knowledge.set("testgame", "game_native_plugins", "Skyrim.esm");

  engine::GameCapabilities caps;
  engine::CapabilityInfo plugins_cap;
  plugins_cap.game_id      = "testgame";
  plugins_cap.capability   = "plugins";
  plugins_cap.display_name = "Plugins";
  caps.register_capability(plugins_cap);

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  auto *rp = w.findChild<ui::RightPanel *>();
  REQUIRE(rp != nullptr);
  rp->set_capabilities(&caps);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", game_dir, inst.info().root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  click_tab(rp->tab_widget(), QStringLiteral("Plugins"));
  auto *ptab = rp->plugins_tab();
  REQUIRE(ptab != nullptr);

  const fs::path profile_dir = engine::Instance::from_root(inst.info().root)
                                   .path_for(engine::InstanceKind::Profiles) /
                               "Default";
  REQUIRE(fs::is_directory(profile_dir));

  QTest::mouseClick(find_button(ptab, "pluginBackupBtn"), Qt::LeftButton);
  REQUIRE(pump_until([&] {
    return backup_stamps_of(profile_dir, "plugins.txt").size() == 1;
  }));
  const std::string stamp = backup_stamps_of(profile_dir, "plugins.txt").front();

  // The live lockedorder.txt goes away, so the safety copy has nothing to
  // capture for it. Its backup still exists, so nothing else is wrong.
  std::error_code ec;
  REQUIRE(fs::remove(profile_dir / "lockedorder.txt", ec));
  REQUIRE(!ec);

  const std::string clobbered = "clobbered\n";
  write_file(profile_dir / "plugins.txt", clobbered);
  write_file(profile_dir / "loadorder.txt", clobbered);

  QString report_main;
  QString report_details;
  ui::set_error_presenter_for_tests([&](ui::TaskDialog &dlg) {
    for (const auto *label : dlg.findChildren<QLabel *>())
      report_main += label->text() + "\n";
    if (const auto *edit = dlg.findChild<QPlainTextEdit *>())
      report_details += edit->toPlainText();
  });
  struct RestorePresenter {
    ~RestorePresenter() { ui::set_error_presenter_for_tests({}); }
  } restore_presenter;

  {
    ModalDriver driver;
    driver.arm(QStringLiteral("Restore"));
    QTest::mouseClick(find_button(ptab, "pluginRestoreBtn"), Qt::LeftButton);
    REQUIRE(driver.answered() == 2);
    driver.disarm();
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

  // The two files whose safety copy landed were restored...
  CHECK(read_file(profile_dir / "plugins.txt") != clobbered);
  CHECK(read_file(profile_dir / "loadorder.txt") != clobbered);
  // ...and the one whose copy failed was SPARED, not created and not written.
  // The post-restore reload must not put it back either: it reloads without
  // writing back, precisely so the promise it just made survives.
  CHECK_FALSE(fs::exists(profile_dir / "lockedorder.txt"));

  INFO("report: " << report_main.toStdString() << report_details.toStdString());
  // NOT the false claim. Two of three files really were replaced, so any
  // sentence saying nothing was overwritten or nothing changed is a lie.
  CHECK_FALSE(report_main.toLower().contains("nothing was overwritten"));
  CHECK_FALSE(report_main.toLower().contains("nothing was changed"));
  // What DID happen, and which file is stale.
  CHECK(report_main.toLower().contains("partly restored"));
  CHECK(report_details.contains("lockedorder.txt"));

  // A restore that partly fails still leaves the state it replaced recoverable:
  // the safety copy holds the pre-restore bytes of both files that changed.
  const auto safety = backup_stamps_of(profile_dir, "loadorder.txt");
  REQUIRE(safety.size() == 2);
  const std::string safety_stamp = safety.back();
  REQUIRE(safety_stamp != stamp);
  CHECK(read_file(profile_dir / ("plugins.txt." + safety_stamp)) == clobbered);
  CHECK(read_file(profile_dir / ("loadorder.txt." + safety_stamp)) == clobbered);

  fs::remove_all(root);
}

// The mod-list pair: same shape, one file. The in-memory profile state must not
// clobber the restore within the debounce window.
// ---------------------------------------------------------------------------
TEST_CASE("mod list Backup and Restore buttons round-trip modlist.txt",
          "[ui][harness][backup]") {
  const fs::path root = make_case_root("czc0_modlist");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "backup_restore_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto inst           = engine::Instance::installed("TestGame", root / "instances");
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());
  const fs::path inst_root = inst.info().root;
  const fs::path mods_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Mods);
  fs::create_directories(mods_dir / "Foo_mod");
  write_file(mods_dir / "Foo_mod" / "meta.ini", "[General]\npriority=0\n");

  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");

  ui::MainWindow w;
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", {}, inst_root);
  REQUIRE(pump_until([&w] {
    return !w.is_loading();
  }));

  const fs::path profile_dir =
      engine::Instance::from_root(inst_root).path_for(engine::InstanceKind::Profiles) /
      "Default";
  REQUIRE(fs::is_directory(profile_dir));

  // 1. Backup the mod list through the real button. The flush happens first,
  //    so the copy is of the current in-memory list, not of whatever the ~5s
  //    debounced writer last emitted.
  QTest::mouseClick(find_button(&w, "modBackupBtn"), Qt::LeftButton);
  REQUIRE(pump_until([&] {
    return backup_stamps_of(profile_dir, "modlist.txt").size() == 1;
  }));
  const std::string stamp     = backup_stamps_of(profile_dir, "modlist.txt").front();
  const std::string backed_up = read_file(profile_dir / ("modlist.txt." + stamp));
  CHECK(backed_up.find("Foo_mod") != std::string::npos);
  CHECK(read_file(profile_dir / ("modlist.txt." + stamp)) ==
        read_file(profile_dir / "modlist.txt"));

  // 2. Change the live mod list.
  const std::string clobbered = "# clobbered\r\n+Foo_mod\r\n";
  write_file(profile_dir / "modlist.txt", clobbered);

  // 3. Restore, and accept. The restore cancels any pending debounced modlist
  //    write before it writes, so the app's own writer cannot clobber these
  //    bytes afterwards.
  {
    ModalDriver driver;
    driver.arm(QStringLiteral("Restore"));
    QTest::mouseClick(find_button(&w, "modRestoreBtn"), Qt::LeftButton);
    REQUIRE(driver.answered() == 2);
    driver.disarm();
  }
  REQUIRE(pump_until([&] {
    return read_file(profile_dir / "modlist.txt") == backed_up;
  }));

  // The safety backup holds what the restore replaced.
  const auto safety = backup_stamps_of(profile_dir, "modlist.txt");
  REQUIRE(safety.size() == 2);
  CHECK(read_file(profile_dir / ("modlist.txt." + safety.back())) == clobbered);

  fs::remove_all(root);
}

// The confirmation itself, without a window: the file list has to name what is
// about to be replaced, and the promise that it is backed up first has to be on
// screen (it is the only thing standing between the user and an unrecoverable
// mistake).
TEST_CASE("restore confirmation names the files and the safety backup",
          "[ui][backup]") {
  const fs::path root = make_case_root("czc0_confirm");

  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "backup_restore_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  ui::TaskDialog dlg;
  ui::configure_restore_backup_dialog(dlg, QObject::tr("load order"),
                                      {QStringLiteral("plugins.txt"),
                                       QStringLiteral("loadorder.txt"),
                                       QStringLiteral("lockedorder.txt")},
                                      QStringLiteral("2026_10_02_08_45_55"));

  CHECK(dlg.windowTitle().contains("Restore"));

  QString labels;
  for (const auto *label : dlg.findChildren<QLabel *>())
    labels += label->text() + "\n";
  INFO("labels: " << labels.toStdString());
  // What is being replaced, and from when.
  CHECK(labels.contains("load order"));
  CHECK(labels.contains("2026_10_02_08_45_55"));
  // The safety backup has to be PROMISED on screen, not merely performed: it is
  // the only thing standing between the user and an unrecoverable mistake.
  CHECK(labels.contains("backed up first"));

  auto *details = dlg.findChild<QPlainTextEdit *>();
  REQUIRE(details != nullptr);
  const QString listed = details->toPlainText();
  INFO("details: " << listed.toStdString());
  CHECK(listed.contains("plugins.txt"));
  CHECK(listed.contains("loadorder.txt"));
  CHECK(listed.contains("lockedorder.txt"));

  QString links;
  // The command links are built inside exec(), so they can only be read from
  // inside its event loop (the task_dialog_test.cpp run_dialog pattern). The
  // answer is picked at the same time: Cancel maps to No, which is the branch
  // the restore path takes when the user refuses.
  QTimer::singleShot(0, &dlg, [&] {
    for (const auto *button : dlg.findChildren<QCommandLinkButton *>())
      links += button->text() + "\n";
    for (auto *button : dlg.findChildren<QCommandLinkButton *>()) {
      if (button->text() == QStringLiteral("Cancel")) {
        button->click();
        return;
      }
    }
    dlg.reject();
  });
  QTimer::singleShot(8000, &dlg, [&dlg] {
    dlg.reject();
  });
  CHECK(dlg.exec() == QMessageBox::No);
  INFO("links: " << links.toStdString());
  CHECK(links.contains("Restore"));
  CHECK(links.contains("Cancel"));

  fs::remove_all(root);
}
