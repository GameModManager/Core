// The cross-profile mod rename must never half-apply SILENTLY.
//
// Renaming the mod folder orphans the entry in every profile but the active
// one, so rename_mod_in_all_profiles() rewrites every modlist.txt. When one of
// them cannot be written the old code logged at ERROR, continued, returned the
// count of the ones that worked, and the caller logged that count at DEBUG -
// so with 5 profiles and profile 3 unwritable the user saw nothing, and
// profile 3 then resolved the mod as MISSING: it dropped out of the list or
// came back at default priority, losing its load order AND its enabled state.
// That is exactly the damage the feature exists to prevent.
//
// This drives the REAL ModActions::apply_rename against a REAL MainWindow with
// a REAL instance, and three REAL profiles on disk:
//
//   Alpha - writable throughout; the control.
//   Zeta  - its profile DIRECTORY is read-only, so safe_write_file()'s temp
//           file cannot be created and the write fails.
//   Yotta - its modlist.txt is unreadable (mode 000), which the old code
//           conflated with an empty modlist and skipped with no log at all.
//
// Asserts the warning the user actually gets names Zeta and Yotta and not
// Alpha. Then it repairs both profiles (exactly what the warning tells the
// user to do), renames again, and asserts NO dialog appears - so a warning
// that fires on a clean rename cannot pass this test.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME / XDG_DATA_HOME, no
// network.
#include "engine/game/registry/game_knowledge.h"
#include "engine/instance/instance.h"
#include "ui/controllers/mod_actions.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMessageBox>
#include <QThread>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char *kOldId = "Old_Mod";
constexpr const char *kNewId = "New_Mod";
constexpr const char *kThird = "Third_Mod";
constexpr const char *kAlpha = "Alpha";
constexpr const char *kZeta  = "Zeta";
constexpr const char *kYotta = "Yotta";
constexpr const char *kRoot  = "/tmp/gmm_rename_profiles_warning";

// A failed run must never leave a read-only directory behind: the next run
// remove_all()s the root, and that cannot unlink inside a mode-0555 directory.
void make_writable(const fs::path &p) {
  std::error_code ec;
  fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, ec);
  if (fs::is_directory(p, ec))
    for (const auto &e : fs::directory_iterator(p, ec))
      make_writable(e.path());
}

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

// Watches for the QMessageBox::warning(w) that apply_rename raises, records
// what it says, and closes it. QMessageBox::warning blocks, so this has to
// poll from a timer while its own modal loop spins. The watchdog closes the
// box whether or not it was ever seen, so a regression fails an assertion
// instead of hanging the suite.
struct DialogWatch {
  QString text;
  int seen = 0;
  QTimer poller;

  explicit DialogWatch(QWidget *host) {
    auto grab = [this, host](bool record) {
      for (auto *box : host->findChildren<QMessageBox *>()) {
        if (record) {
          text = box->text();
          ++seen;
        }
        box->accept();
        return true;
      }
      return false;
    };
    poller.setInterval(10);
    QObject::connect(&poller, &QTimer::timeout, &poller, [grab] {
      grab(true);
    });
    poller.start();
    QTimer::singleShot(20000, &poller, [grab] {
      grab(false);
    });
  }

  ~DialogWatch() { poller.stop(); }
};

}  // namespace

TEST_CASE("a mod rename that misses profiles says so", "[ui][profiles]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = kRoot;
  make_writable(root);
  fs::remove_all(root);
  fs::create_directories(root / "config");
  fs::create_directories(root / "data");
  fs::create_directories(root / "instances");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));
  qputenv("XDG_DATA_HOME", QByteArray((root / "data").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "rename_profiles_warning_test";
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
  const fs::path profiles  = instance.path_for(engine::InstanceKind::Profiles);
  fs::create_directories(mods_dir / kOldId);
  write_file(mods_dir / kOldId / "meta.ini", "[General]\n");

  // Three profiles that all list the mod. The line order is its load order and
  // the '+' is its enabled state - what a missed profile loses.
  for (const char *name : {kAlpha, kZeta, kYotta}) {
    fs::create_directories(profiles / name);
    write_file(profiles / name / "modlist.txt",
               std::string("+") + kOldId + "\n+Something Else\n");
  }

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
    return find_mod_row(model, kOldId) >= 0;
  }));

  // Break two profiles. The file mode first, then the directory mode, so each
  // chmod still has the access it needs.
  fs::permissions(profiles / kYotta / "modlist.txt", fs::perms::none);
  fs::permissions(profiles / kZeta, fs::perms::owner_read | fs::perms::owner_exec);

  // ---- the rename that half-applies ---------------------------------------
  int row = find_mod_row(model, kOldId);
  REQUIRE(row >= 0);
  ui::ModActions actions(&w);

  {
    DialogWatch watch(&w);
    actions.apply_rename(row, kNewId);

    // Readable again so the damage below can be read back. The rename has
    // already run - this cannot rescue the write that just failed.
    fs::permissions(profiles / kYotta / "modlist.txt", fs::perms::owner_all);

    // The folder really did move - this is a rename that happened, not a
    // refusal.
    REQUIRE_FALSE(fs::exists(mods_dir / kOldId));
    REQUIRE(fs::exists(mods_dir / kNewId));
    // The healthy profile really did get the new name.
    REQUIRE(read_file(profiles / kAlpha / "modlist.txt") ==
            std::string("+") + kNewId + "\n+Something Else\n");
    // The two broken ones still name the old folder, which is the damage.
    REQUIRE(read_file(profiles / kZeta / "modlist.txt") ==
            std::string("+") + kOldId + "\n+Something Else\n");
    REQUIRE(read_file(profiles / kYotta / "modlist.txt") ==
            std::string("+") + kOldId + "\n+Something Else\n");

    // THE POINT: exactly one dialog, naming the two profiles that missed out
    // and not the one that worked.
    REQUIRE(watch.seen == 1);
    CHECK(watch.text.contains(kZeta));
    CHECK(watch.text.contains(kYotta));
    CHECK_FALSE(watch.text.contains(kAlpha));
    // It has to say what the damage is, not just that something went wrong.
    CHECK(watch.text.contains("load order"));
  }

  // Repair exactly what the warning tells the user to do - re-add the mod in
  // the two profiles it missed - then rename once more. Nothing fails, so
  // nothing is reported: a warning that fires on a clean rename cannot pass.
  fs::permissions(profiles / kZeta, fs::perms::owner_all);
  write_file(profiles / kZeta / "modlist.txt", std::string("+") + kNewId + "\n");
  write_file(profiles / kYotta / "modlist.txt", std::string("+") + kNewId + "\n");

  row = find_mod_row(model, kNewId);
  REQUIRE(row >= 0);
  {
    DialogWatch clean(&w);
    actions.apply_rename(row, kThird);
    CHECK(clean.seen == 0);
  }
  REQUIRE(read_file(profiles / kZeta / "modlist.txt") ==
          std::string("+") + kThird + "\n");

  make_writable(root);
}
