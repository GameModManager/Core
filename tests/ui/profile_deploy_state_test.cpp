// Switching profile must move the DEPLOY SET, not just the checkbox column.
//
// The defect: apply_profile_mod_states() applied the incoming profile to the
// in-memory model and then re-applied the on-disk sentinel over the top of it
// ("the global disable sentinel always wins"), so a profile's enable/disable
// never reached disk. deploy_utils.cpp:389 reads ONLY the sentinel, so the two
// halves disagreed and the profile switch was decorative.
//
// So this asserts on the deploy set, computed by running the real deploy
// strategy over the same mods dir with the same disable mechanism and the same
// deploy prefix the plugin declares. A model-only assertion passes while the
// bug stands - the checkbox and the game disagree exactly in that case - which
// is why nothing here looks at the model.
//
// Four mods, four starting states on disk, one switch:
//
//   ModA  both profiles say +   sentinel PRESENT  -> switch must REMOVE it
//   ModB  outgoing - / incoming +, sentinel ABSENT -> already right, no write
//   ModC  no profile lists it    sentinel ABSENT   -> switch must CREATE it
//   ModD  incoming says -        sentinel PRESENT  -> already right, no write
//
// ModA is the case that killed the previous guard: it compared the incoming
// profile against the OUTGOING profile's modlist.txt, so two profiles agreeing
// that ModA is on meant "no change" - while the sentinel, which is the only
// thing the deploy reads, still said off. ModA stayed out of the deploy set
// forever. The guard now reads the sentinel, so disagreeing with it is exactly
// what makes a mod get written.
//
// ModC is MO2's rule: Profile::ModStatus defaults to m_Enabled(false)
// (profile.h:379) and refreshModStatus hands an unlisted mod a priority without
// setting m_Enabled, so it never reaches getActiveMods(). It starts on disk
// with NO sentinel, i.e. deploying - so if the switch inherits the disk instead
// of disabling it, ModC lands in the deploy set and the assertion goes red.
//
// ModD carries a sentinel that is already in the state the profile asks for.
// It is a DIRECTORY, so a write cannot succeed: create_empty_file on a path
// whose parent is a directory fails with EISDIR whatever the caller runs as.
// The only way ModD's row can pick up a toggle error is if the switch actually
// tried to rewrite it. That is the cheapness assertion - it proves the write
// did not happen rather than inferring it from the result being right.
//
// Driven through the real MainWindow against the REAL Isaac plugin, so the
// modlist.txt parse, the sentinel name and the mods_subpath resolution are the
// shipped ones rather than hand-written constants.

#include "engine/deploy/strategy_direct.h"
#include "engine/instance/instance.h"
#include "engine/plugin_host/plugin_loader.h"
#include "platform/platform.h"
#include "test_platform.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *kGameId   = "TheBindingOfIsaacRebirth";
constexpr const char *kSentinel = "disable.it";

#ifndef GMM_ISAAC_PLUGIN_PATH
#define GMM_ISAAC_PLUGIN_PATH "TheBindingOfIsaacRebirth.so"
#endif

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

// Per-case throwaway root, removed on scope exit even when an assertion FAILs.
// HOME / XDG_CONFIG_HOME / XDG_DATA_HOME all point into it, so Settings and
// the freedesktop trash stay out of the real ones - the real
// GameModManager.conf has been damaged by unsandboxed runs before.
struct CaseRoot {
  explicit CaseRoot(const char *name) : root(fs::path("/tmp") / name) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "config");
    fs::create_directories(root / "data");
    fs::create_directories(root / "instances");
    fs::create_directories(root / "home");
    qputenv("HOME", QByteArray((root / "home").string().c_str()));
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

void write_file(const fs::path &p, const std::string &content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << content;
}

struct IsaacWorld {
  explicit IsaacWorld(const char *tmp_name) : case_root(tmp_name) {}
  CaseRoot case_root;
  engine::PluginLoader loader;
  FakePlatform platform{case_root.root / "data"};
  ui::MainWindow w;
  fs::path inst_root;
  fs::path mods_dir;
  fs::path profiles_dir;
  fs::path game_dir;

  void setup_dirs() {
    auto inst = engine::Instance::installed(kGameId, case_root.root / "instances");
    inst.info().game_id = kGameId;
    REQUIRE(inst.create_directories());
    REQUIRE(inst.write_toml());
    inst_root    = inst.info().root;
    const auto i = engine::Instance::from_root(inst_root);
    mods_dir     = i.path_for(engine::InstanceKind::Mods);
    profiles_dir = i.path_for(engine::InstanceKind::Profiles);
    game_dir     = case_root.root / "game";
    fs::create_directories(game_dir);
  }

  // A mod folder holding one file under the game's mods_subpath, so the deploy
  // below has something that can only land there if the mod is in the set.
  void lay_mod(const std::string &id) {
    fs::create_directories(mods_dir / id / "mods");
    write_file(mods_dir / id / "meta.ini",
               "[General]\nname=" + id + "\nversion=1.0\npriority=0\n");
    write_file(mods_dir / id / "mods" / (id + ".dat"), "payload-" + id);
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

  [[nodiscard]] bool sentinel_present(const std::string &id) const {
    std::error_code ec;
    return fs::exists(mods_dir / id / kSentinel, ec);
  }

  // The toggle-error badge a row carries, empty when the last write to its
  // sentinel succeeded or was never attempted. Read off the model because
  // that is where a failed sentinel write surfaces.
  [[nodiscard]] QString toggle_error_for(const std::string &id) const {
    auto *model = w.findChild<ui::ModList *>();
    REQUIRE(model != nullptr);
    for (const auto &m : model->mods()) {
      if (m.id == QString::fromStdString(id))
        return m.toggle_error;
    }
    return QStringLiteral("<no such row>");
  }

  // What the deploy would actually do, computed the way the deploy computes
  // it: walk the mods dir, skip the folder carrying the sentinel, and record
  // what it deployed. Read back out of the deploy's OWN ledger rather than a
  // hardcoded destination path, so this stays honest if the deploy's path
  // layout changes and cannot be satisfied by a file that merely exists.
  //
  // The deploy prefix and the mod-id-inclusion come from the plugin, not from
  // constants here.
  [[nodiscard]] std::set<std::string> deploy_set() const {
    const auto &knowledge = loader.knowledge();
    const fs::path stage  = case_root.root / "deploy";

    engine::DirectDeployStrategy::Config cfg;
    cfg.mods_dir      = mods_dir;
    cfg.game_dir      = stage / "game";
    cfg.deploy_prefix = knowledge.get(kGameId, "deploy_prefix", "mods");
    cfg.deploy_include_mod_id =
        knowledge.get(kGameId, "deploy_include_mod_id", "true") == "true";
    cfg.disable_mechanism = knowledge.get(kGameId, "disable_mechanism", kSentinel);
    cfg.case_sensitive    = true;
    cfg.ledger_file       = stage / "ledger";
    cfg.backup_root       = stage / "Original_Files";
    fs::create_directories(cfg.game_dir);
    const fs::path ledger = cfg.ledger_file;  // cfg moves below
    engine::DirectDeployStrategy strategy(std::move(cfg));
    REQUIRE(strategy.deploy_all());

    // target -> source. The source still names the mod folder it came from, so
    // the first path segment under mods_dir IS the mod that deployed.
    std::set<std::string> deployed;
    for (const auto &[target, source] : engine::load_deploy_ledger(ledger)) {
      const auto rel = source.lexically_relative(mods_dir);
      if (rel.empty() || *rel.begin() == "..")
        continue;
      deployed.insert(*rel.begin());
    }
    std::error_code ec;
    fs::remove_all(stage, ec);
    return deployed;
  }
};

}  // namespace

TEST_CASE("profile switch: the deploy set follows the new profile",
          "[ui][isaac][profile]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int app_argc     = 1;
  char app_argv0[] = "profile_deploy_state_test";
  char *app_argv[] = {app_argv0, nullptr};
  QApplication app(app_argc, app_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  IsaacWorld world("gmm_profile_deploy_state");
  world.setup_dirs();

  for (const char *id : {"ModA", "ModB", "ModC", "ModD"})
    world.lay_mod(id);

  // The outgoing profile (Default) and the incoming one (Combat). They AGREE
  // about ModA: both list it enabled. That is what makes ModA the case the
  // previous guard got wrong - keyed on the outgoing modlist, agreement read as
  // "nothing to do" while the sentinel still said off.
  // ModC is deliberately absent from both.
  write_file(world.profiles_dir / "Default" / "modlist.txt",
             "# GameModManager\n+ModA\n-ModB\n");
  fs::create_directories(world.profiles_dir / "Combat");
  write_file(world.profiles_dir / "Combat" / "modlist.txt",
             "# GameModManager\n+ModA\n+ModB\n-ModD\n");

  // Starting state on disk, which is what the deploy has been following all
  // along: ModA off, ModB on, ModC on, ModD off.
  write_file(world.mods_dir / "ModA" / kSentinel, "");
  // ModD's sentinel is a directory on purpose: it is already in the state the
  // incoming profile asks for, and a write over it cannot succeed (EISDIR on
  // open(O_WRONLY) whatever the process runs as). So the row picking up a
  // toggle error is proof the switch tried to rewrite it, and the row staying
  // clean is proof it did not.
  fs::create_directories(world.mods_dir / "ModD" / kSentinel);
  REQUIRE(world.sentinel_present("ModA"));
  REQUIRE_FALSE(world.sentinel_present("ModB"));
  REQUIRE_FALSE(world.sentinel_present("ModC"));
  REQUIRE(world.sentinel_present("ModD"));

  world.open_window();

  CHECK(world.deploy_set() == std::set<std::string>{"ModB", "ModC"});

  // Synchronous: the engine switcher restores the new profile's modlist and the
  // write-through runs on this thread before switch_profile returns. The scan it
  // kicks off lands later and must not be waited on - it would only re-apply the
  // sentinel, which is what the assertions below are about.
  world.w.switch_profile(QStringLiteral("Combat"));

  // The decisive assertion: what deploys is not the checkbox column. ModC is
  // in no modlist.txt, so MO2's rule disables it and it leaves the deploy set
  // rather than inheriting "no sentinel, so on" from the disk.
  CHECK(world.deploy_set() == std::set<std::string>{"ModA", "ModB"});

  // ModA the profile enables reaches disk (the sentinel it shipped with is
  // gone). Both profiles already said enabled, so only a guard that reads the
  // SENTINEL rather than the outgoing modlist gets here.
  CHECK_FALSE(world.sentinel_present("ModA"));
  // ModB was already on disk without a sentinel and the profile keeps it on, so
  // there is nothing to write.
  CHECK_FALSE(world.sentinel_present("ModB"));

  // ModC is in no modlist.txt: MO2's ModStatus default is disabled, so the
  // switch writes the sentinel the disk was missing.
  CHECK(world.sentinel_present("ModC"));

  // ModD already matches the profile. Its sentinel cannot be written, so a
  // clean row is the only way to know the switch left it alone - this is the
  // cheapness claim, asserted as "the write did not happen" rather than
  // inferred from the state being right.
  CHECK(world.toggle_error_for("ModD").isEmpty());
}