// source_tab_test.cpp — P8.3 regression for the Mod Info -> Source tab's
// async Refresh.
//
// The Nexus mod-info fetch (network round-trip + JSON parse) must run off the
// UI thread: clicking Refresh must never block behind a WaitCursor. We prove
// that by parking a fake fetch_nexus_info on a QSemaphore — a first click
// parks the worker, and the test can still click Refresh again (which would
// deadlock if on_refresh ran the fetch synchronously). A second Refresh while
// one is in flight must supersede the first cleanly: the stale result is
// dropped (generation mismatch) and only the newer fetch's data lands in the
// mod's meta (no torn write). Hermetic: no network, no real config (fake
// provider registered in SourceRegistry, XDG_CONFIG_HOME pointed at a
// throwaway dir).
#include "engine/mod/meta/mod_meta.h"
#include "engine/source/nexus/provider.h"
#include "engine/source/interface.h"
#include "engine/source/registry.h"
#include "ui/modinfo/description_renderer.h"
#include "ui/modinfo/mod_info_dialog.h"
#include "ui/modinfo/source_panels/git_source_panel.h"
#include "ui/modinfo/source_panels/nexus_source_panel.h"
#include "ui/modinfo/source_panels/steam_source_panel.h"
#include "ui/modinfo/source_tab.h"
#include "ui/theme/icon_manager.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSemaphore>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

// A Nexus-typed provider with no network surface; find_provider() in
// source_tab.cpp matches its display name and source_type()=="nexus" routes
// it to the full metadata form.
struct FakeNexusProvider : engine::Source::Interface {
  std::string source_type() const override { return "nexus"; }
  bool fetch(const engine::Mod &, engine::PipelineContext &,
             const std::filesystem::path &) override {
    return false;
  }
  std::string display_name() const override { return "Test Nexus"; }
};

static QPushButton *find_refresh(QWidget &w) {
  for (auto *b : w.findChildren<QPushButton *>())
    if (b->text() == QLatin1String("Refresh"))
      return b;
  return nullptr;
}

static bool wait_for(const std::function<bool()> &pred, int timeout_ms = 5000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    if (pred())
      return true;
    QThread::msleep(10);
  }
  return pred();
}

// One QApplication for the whole process, not one per TEST_CASE. Chromium's
// default QWebEngineProfile (built lazily by the first Nexus panel via
// create_description_renderer) is a process-wide singleton that outlives a
// stack-local QApplication; once its internals are initialized, the next
// case's QWebEnginePage construction dereferences state owned by the
// destroyed first app and SEGVs inside libQt6WebEngineCore. Catch2 runs all
// three cases in one binary, so the app must live as long as the process -
// same pattern description_browser_test already uses. Heap-allocated and
// intentionally never deleted: destroying it during exit() crashes in
// ~QApplication -> qt_call_post_routines once Chromium's globals are
// already torn down. LSan does not report it - QCoreApplication::self
// (a global in libQt6Widgets) keeps it reachable.
static QApplication &shared_app() {
  static int argc     = 1;
  static char argv0[] = "source_tab_test";
  static char *argv[] = {argv0, nullptr};
  // Offscreen tests never render to a real surface, so Chromium's GPU stack
  // has to be out: with the app kept alive to exit, the GPU thread outlives
  // the test and calls vkCreateInstance during process teardown, which
  // crashes inside system Vulkan layers (MangoHud on this machine). That is
  // the app's own configuration, applied by a static initialiser in
  // description_renderer.cpp before any Qt object exists - deliberately not
  // repeated here, so a test binary cannot end up rendering under different
  // flags than the app ships. The next test case asserts it is really there.
  static QApplication *app = new QApplication(argc, argv);
  return *app;
}

static ui::ModInfoData
make_data(const std::string &id,
          std::function<engine::Source::Nexus::ModInfoResult()> fetch,
          const std::filesystem::path &mods_dir) {
  ui::ModInfoData data;
  data.id   = QString::fromStdString(id);
  data.name = QString::fromStdString(id);
  // Source tab (Workspace-fqf5) only renders a Nexus panel when the mod
  // is actually Nexus-sourced. Test fixtures that exercise the Nexus
  // panel therefore mark source_type="nexus" so the panel builds. See
  // "source tab has_data and single source" below for the manual / non-
  // Nexus paths.
  data.source_type       = QStringLiteral("nexus");
  data.source_id         = QStringLiteral("42");
  data.nexus_domain      = QStringLiteral("testgame");
  data.supported_sources = QStringList{QStringLiteral("Test Nexus")};
  data.fetch_nexus_info  = std::move(fetch);
  data.load_meta         = [mods_dir, id] {
    return engine::ModMeta::load(mods_dir, id);
  };
  data.save_meta = [mods_dir, id](const engine::ModMeta &m) {
    return m.save(mods_dir, id);
  };
  return data;
}

// Helper for the manual-mod fixtures used in the single-source tests.
static ui::ModInfoData make_manual_data(const std::string &id,
                                        const std::filesystem::path &mods_dir) {
  ui::ModInfoData data;
  data.id                = QString::fromStdString(id);
  data.name              = QString::fromStdString(id);
  data.source_type       = QStringLiteral("manual");
  data.source_id         = QString();
  data.nexus_domain      = QString();
  data.supported_sources = QStringList{QStringLiteral("Test Nexus")};
  data.load_meta         = [mods_dir, id] {
    return engine::ModMeta::load(mods_dir, id);
  };
  data.save_meta = [mods_dir, id](const engine::ModMeta &m) {
    return m.save(mods_dir, id);
  };
  return data;
}

TEST_CASE("source tab", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_source_tab/config";
  std::filesystem::remove_all("/tmp/gmm_source_tab");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();  // process-lifetime QApplication (see above)
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path instance = "/tmp/gmm_source_tab/instances/Test";
  const std::filesystem::path mods_dir = instance / "mods";
  std::filesystem::create_directories(mods_dir);

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // --- Scenario 1: a plain Refresh fetches off the main thread and lands. ---
  {
    std::atomic<int> calls      = 0;
    std::atomic<bool> on_worker = false;
    ui::SourceTab tab;
    auto data1 = make_data(
        "ModA",
        [&]() -> engine::Source::Nexus::ModInfoResult {
          ++calls;
          on_worker = QThread::currentThread() != qApp->thread();
          engine::Source::Nexus::ModInfoResult r;
          r.available      = true;
          r.name           = "Fetched Mod";
          r.version        = "2.0";
          r.newest_version = "2.1";
          r.category_id    = "7";
          r.description    = "Fetched description";
          return r;
        },
        mods_dir);
    tab.set_current(data1);
    tab.set_mod(data1);
    tab.first_activation();
    QApplication::processEvents();

    auto *refresh = find_refresh(tab);
    if (!refresh) {
      std::printf("DEBUG buttons:");
      for (auto *b : tab.findChildren<QPushButton *>())
        std::printf(" [%s]", qPrintable(b->text()));
      std::printf("\n");
      for (auto *lbl : tab.findChildren<QLabel *>())
        std::printf("DEBUG label: %s\n", qPrintable(lbl->text()));
    }
    check(refresh != nullptr, "Nexus page has a Refresh button");
    refresh->click();

    const bool landed = wait_for([&] {
      return engine::ModMeta::load(mods_dir, "ModA")
                 .get("Nexusmods", "nexusdescription") == "Fetched description";
    });
    check(landed, "refresh result persisted to the mod's meta");
    check(on_worker, "fetch ran on the worker thread, not the UI thread");
    check(calls == 1, "one refresh = exactly one fetch");

    auto *refresh2 = find_refresh(tab);
    check(refresh2 != nullptr && refresh2->isEnabled() &&
              refresh2->text() == QLatin1String("Refresh"),
          "Refresh button re-enabled after the result lands");

    bool desc_shown = false;
    for (auto *renderer : tab.findChildren<ui::DescriptionRenderer *>())
      if (renderer->current_description().contains("Fetched description"))
        desc_shown = true;
    check(desc_shown, "description renderer shows the fetched text");

    bool ver_shown = false;
    for (auto *le : tab.findChildren<QLineEdit *>())
      if (le->text() == QStringLiteral("2.0"))
        ver_shown = true;
    check(ver_shown, "version field shows the fetched version");
  }

  // --- Scenario 2: a second Refresh while one is in flight supersedes it. ---
  {
    QSemaphore gate(0);
    std::atomic<int> calls      = 0;
    std::atomic<bool> on_worker = false;
    ui::SourceTab tab;
    auto data2 = make_data(
        "ModB",
        [&]() -> engine::Source::Nexus::ModInfoResult {
          ++calls;
          on_worker = QThread::currentThread() != qApp->thread();
          if (calls == 1)
            gate.tryAcquire(1, 5000);  // park the worker
          engine::Source::Nexus::ModInfoResult r;
          r.available   = true;
          r.description = (calls == 2) ? "second result" : "first result";
          return r;
        },
        mods_dir);
    tab.set_current(data2);
    tab.set_mod(data2);
    tab.first_activation();
    QApplication::processEvents();

    auto *refresh = find_refresh(tab);
    REQUIRE(refresh != nullptr);

    // First click: gen 1, the worker parks inside the fetch.
    refresh->click();
    check(wait_for(
              [&] {
                return calls == 1;
              },
              2000),
          "first fetch started and parked on the worker");

    // The in-flight fetch disables the button (real UI affordance).
    check(!refresh->isEnabled() && refresh->text() == QStringLiteral("Fetching…"),
          "Refresh disabled while a fetch is in flight");

    // Second click while the first is in flight must NOT block — a
    // synchronous refresh would deadlock here (the worker is parked).
    // The disabled guard blocks real clicks; drive the coalescing path
    // by re-enabling first (on_refresh re-disables it immediately).
    refresh->setEnabled(true);
    refresh->click();

    // Release the parked fetch: its stale result must be dropped and a
    // follow-up fetch launched with the newer generation.
    gate.release();
    const bool landed = wait_for([&] {
      return engine::ModMeta::load(mods_dir, "ModB")
                 .get("Nexusmods", "nexusdescription") == "second result";
    });
    check(landed, "second refresh supersedes the first (stale result dropped)");
    check(on_worker, "superseded fetch also ran on the worker thread");
    INFO("calls=" << calls.load());
    check(calls == 2, "coalesced: exactly two fetches, no third");
    check(
        engine::ModMeta::load(mods_dir, "ModB").get("Nexusmods", "nexusdescription") ==
            "second result",
        "meta holds only the newer result (no torn write)");
  }
}

// Source-attribution regression for the Source tab (Workspace-rvld +
// Workspace-fqf5).
//
// Three bugs are guarded here:
//
//   1) NexusSourcePanel::has_data() used to return true whenever any of
//      {data_.source_id, [General]version, [Nexusmods]modid} was
//      non-empty. Every install writes a version (default 1.0), so
//      has_data() was ALWAYS true - the Source tab's red-dot in the
//      mod list fired for every mod, manual or otherwise, and any
//      "Nexus has data" aggregation in SourceTab::set_mod() was
//      polluted. The fix checks source_type=="nexus" OR a non-zero
//      [Nexusmods]modid.
//
//   2) SourceTab::populate() used to derive the visible source tabs
//      ONLY from the game's knowledge-hook "download_sources". When
//      Skyrim's hook only names "Nexus", a LoversLab mod that does
//      carry a [LoversLab] section loses its provenance in the UI.
//      The fix unions supported_sources with the sections actually
//      present in the mod's meta so LoversLab / Steam tabs surface
//      whenever the mod actually has that provenance, regardless of
//      the game hook. (Kept after fqf5 only as a fallback for the
//      actual source detection - the game hook no longer FABRICATES
//      a Nexus tab for non-Nexus mods.)
//
//   3) SourceTab::populate() (Workspace-fqf5) used to display a Nexus
//      tab for every mod under a Nexus-enabled game, regardless of the
//      mod's actual source. Users conflated tab visibility with source
//      attribution and assumed every mod was Nexus-sourced. The fix
//      shows exactly ONE source tab (= the mod's actual source) plus
//      a "+" affordance that opens an add-source dialog. Manual mods
//      show a "Manual" placeholder and no Nexus tab.
TEST_CASE("source tab has_data and single source", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_source_tab_union/config";
  std::filesystem::remove_all("/tmp/gmm_source_tab_union");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();  // process-lifetime QApplication (see above)
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path instance = "/tmp/gmm_source_tab_union/instances/Test";
  const std::filesystem::path mods_dir = instance / "mods";
  std::filesystem::create_directories(mods_dir);

  // Register the same fake Nexus provider the other scenario uses so
  // find_provider() can match "Test Nexus".
  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // --- Scenario 1: a manual mod shows the "Manual" placeholder, NOT
  // a Nexus panel (Workspace-fqf5). The "+" affordance tab is the
  // only thing after the placeholder. ---
  {
    engine::ModMeta manual;
    manual.set("General", "version", "1.0");
    manual.set("GameModManager", "source_type", "manual");
    manual.save(mods_dir, "ManualMod");

    auto data = make_manual_data("ManualMod", mods_dir);

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    // The tab bar has exactly one content tab (the Manual placeholder)
    // plus the "+" affordance.
    auto *qtw = tab.findChild<QTabWidget *>();
    check(qtw != nullptr, "SourceTab owns a QTabWidget");
    check(qtw && qtw->count() == 2,
          "manual mod: 2 tabs (Manual placeholder + '+' affordance)");

    // The Nexus panel is NOT created - this is the core fix.
    ui::NexusSourcePanel *nexus = nullptr;
    for (auto *p : tab.findChildren<ui::NexusSourcePanel *>())
      nexus = p;
    check(nexus == nullptr,
          "manual mod: no Nexus panel created (only the actual source)");

    // The last tab is the "+" affordance - title "+".
    check(qtw && qtw->tabText(qtw->count() - 1) == QLatin1String("+"),
          "manual mod: last tab is the '+' affordance");
  }

  // --- Scenario 2: a Nexus-sourced mod shows exactly one Nexus panel
  // plus the "+" affordance. The Nexus panel reports has_data()==true. ---
  {
    engine::ModMeta nexus_mod;
    nexus_mod.set("General", "version", "1.0");
    nexus_mod.set("GameModManager", "source_type", "nexus");
    nexus_mod.set("GameModManager", "source_id", "12345");
    nexus_mod.set("Nexusmods", "modid", "12345");
    nexus_mod.save(mods_dir, "NexusMod");

    ui::ModInfoData data;
    data.id                = QStringLiteral("NexusMod");
    data.name              = QStringLiteral("NexusMod");
    data.source_type       = QStringLiteral("nexus");
    data.source_id         = QStringLiteral("12345");
    data.nexus_domain      = QStringLiteral("testgame");
    data.supported_sources = QStringList{QStringLiteral("Test Nexus")};
    data.load_meta         = [mods_dir] {
      return engine::ModMeta::load(mods_dir, "NexusMod");
    };
    data.save_meta = [mods_dir](const engine::ModMeta &m) {
      return m.save(mods_dir, "NexusMod");
    };

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    check(qtw && qtw->count() == 2, "nexus mod: 2 tabs (Nexus + '+' affordance)");

    ui::NexusSourcePanel *nexus = nullptr;
    for (auto *p : tab.findChildren<ui::NexusSourcePanel *>())
      nexus = p;
    check(nexus && nexus->has_data(), "nexus mod: Nexus panel has_data()==true");
  }

  // --- Scenario 3: a mod with [Nexusmods]modid="0" but
  // source_type="manual" - the MO2 "no Nexus id" sentinel. No Nexus
  // panel because there is no actual Nexus provenance. ---
  {
    engine::ModMeta fake;
    fake.set("General", "version", "1.0");
    fake.set("GameModManager", "source_type", "manual");
    fake.set("Nexusmods", "modid", "0");
    fake.save(mods_dir, "ZeroModidMod");

    auto data = make_manual_data("ZeroModidMod", mods_dir);

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    check(qtw && qtw->count() == 2,
          "modid=0 sentinel: 2 tabs (Manual + '+' affordance)");

    ui::NexusSourcePanel *nexus = nullptr;
    for (auto *p : tab.findChildren<ui::NexusSourcePanel *>())
      nexus = p;
    check(nexus == nullptr, "modid=0 sentinel: no Nexus panel created");
  }
}

// "+" add-source flow (Workspace-fqf5):
//   Clicking the "+" affordance tab opens a dialog that lets the user
//   attach a Nexus / LoversLab / Steam source to the mod. On confirm,
//   the provider section + canonical source keys are written to meta,
//   the in-memory ModInfoData is updated, and the tab rebuilds with the
//   new source's panel.
//
//   This scenario drives the flow without user interaction by directly
//   calling the test-only hooks we wired in via the existing public
//   surface (set_mod repopulates; the save_meta lambda writes the meta).
//   We then assert the side-effects: meta carries the new keys and the
//   panel count drops from "Manual + '+'" to "Nexus + '+'".
TEST_CASE("source tab add source flow", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_source_tab_add/config";
  std::filesystem::remove_all("/tmp/gmm_source_tab_add");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();  // process-lifetime QApplication (see above)
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path instance = "/tmp/gmm_source_tab_add/instances/Test";
  const std::filesystem::path mods_dir = instance / "mods";
  std::filesystem::create_directories(mods_dir);

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // Start from a manual mod with no source attribution.
  engine::ModMeta initial;
  initial.set("General", "version", "1.0");
  initial.set("GameModManager", "source_type", "manual");
  initial.save(mods_dir, "AddSourceMod");

  auto data = make_manual_data("AddSourceMod", mods_dir);

  ui::SourceTab tab;
  tab.set_current(data);
  tab.set_mod(data);
  tab.first_activation();
  QApplication::processEvents();

  auto *qtw = tab.findChild<QTabWidget *>();
  check(qtw && qtw->count() == 2, "before add: 2 tabs (Manual + '+')");

  // Simulate the user picking Nexus with mod id 99999 in the dialog:
  // mutate the in-memory data, write the meta sidecar via the same
  // save_meta lambda the dialog uses, then ask the tab to repopulate.
  ui::ModInfoData updated = data;
  updated.source_type     = QStringLiteral("nexus");
  updated.source_id       = QStringLiteral("99999");
  updated.nexus_domain    = QStringLiteral("testgame");
  tab.set_current(updated);

  if (updated.load_meta && updated.save_meta) {
    auto meta = updated.load_meta();
    meta.set("GameModManager", "source_type", "nexus");
    meta.set("GameModManager", "source_id", "99999");
    meta.set("Nexusmods", "modid", "99999");
    meta.set("Nexusmods", "mod_id", "99999");
    updated.save_meta(meta);
  }
  tab.set_mod(updated);
  QApplication::processEvents();

  // After the add-source flow: meta carries Nexus provenance, tab
  // shows Nexus panel + "+" (not Manual), and Nexus panel has_data().
  auto loaded = engine::ModMeta::load(mods_dir, "AddSourceMod");
  check(loaded.get("GameModManager", "source_type") == "nexus",
        "after add: meta source_type is nexus");
  check(loaded.get("Nexusmods", "modid") == "99999",
        "after add: meta Nexusmods modid is the dialog value");
  check(loaded.get("Nexusmods", "mod_id") == "99999",
        "after add: meta Nexusmods mod_id alias matches");

  auto *qtw2 = tab.findChild<QTabWidget *>();
  check(qtw2 && qtw2->count() == 2, "after add: 2 tabs (Nexus + '+')");

  ui::NexusSourcePanel *nexus = nullptr;
  for (auto *p : tab.findChildren<ui::NexusSourcePanel *>())
    nexus = p;
  check(nexus != nullptr, "after add: Nexus panel exists");
  check(nexus && nexus->has_data(), "after add: Nexus panel has_data()==true");
}

// Git coexists with a download source: a mod whose folder is a git working
// copy shows a Git tab BESIDE its Nexus tab, and the warning banner appears
// inside the Git panel only because the second source exists. The tab title is
// always "Git" - GitHub is a badge, never a name.
//
// Hermetic: no network, no git invocation. The .git directory is written by
// hand and only the display path (is_repository / remote_url / host) runs,
// so this passes whether or not git is installed.
TEST_CASE("git source coexists with a download source", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_source_tab_git";
  std::filesystem::remove_all(root);
  const std::filesystem::path cfg = root / "config";
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();  // process-lifetime QApplication (see above)
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path mods_dir = root / "instances" / "Test" / "mods";
  std::filesystem::create_directories(mods_dir);

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // --- Git + Nexus: two tabs, Git second, banner shown. ---
  {
    const std::string id                = "GitNexusMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = https://github.com/user/repo.git\n";
    }

    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "nexus");
    meta.set("GameModManager", "source_id", "42");
    meta.set("Nexusmods", "modid", "42");
    meta.save(mods_dir, id);

    ui::ModInfoData data = make_data(id, nullptr, mods_dir);
    data.mod_dir         = QDir(QString::fromStdString(mod_dir.string()));
    data.is_git          = true;
    data.git_remote_url  = QStringLiteral("https://github.com/user/repo.git");

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    REQUIRE(qtw != nullptr);
    // Nexus + Git + "+". The Manual placeholder is gone: the mod HAS a source.
    check(qtw->count() == 3, "git + nexus shows 3 tabs (Nexus, Git, '+')");
    check(qtw->tabText(0) == QLatin1String("Test Nexus (primary)"),
          "the download source keeps its own tab, marked primary over Git");
    check(qtw->tabText(1) == QLatin1String("Git"),
          "the git tab is titled Git, never GitHub");

    bool has_git_panel = false;
    for (auto *p : tab.findChildren<ui::GitSourcePanel *>()) {
      has_git_panel = true;
      check(p->has_data(), "the git panel reports source data");
    }
    check(has_git_panel, "the git panel exists alongside the nexus panel");

    // The banner is what tells the user the two sources can disagree.
    // isVisibleTo(panel), not isVisible(): the Git panel is not the selected
    // tab, so a plain isVisible() is false for the whole page.
    bool banner = false;
    for (auto *p : tab.findChildren<ui::GitSourcePanel *>()) {
      for (auto *l : p->findChildren<QLabel *>()) {
        if (l->isVisibleTo(p) &&
            l->text().contains(QLatin1String("downloaded from a different source")))
          banner = true;
      }
    }
    check(banner, "a coexisting download source shows the git warning banner");
  }

  // --- Git only: one content tab, no Manual placeholder, no banner. ---
  {
    const std::string id                = "GitOnlyMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      // A GitLab remote: the host is unknown to the vendor set, so the
      // generic git badge is the one that must apply.
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = git@gitlab.com:group/proj.git\n";
    }

    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "manual");
    meta.save(mods_dir, id);

    ui::ModInfoData data = make_manual_data(id, mods_dir);
    data.mod_dir         = QDir(QString::fromStdString(mod_dir.string()));
    data.is_git          = true;
    data.git_remote_url  = QStringLiteral("git@gitlab.com:group/proj.git");

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    REQUIRE(qtw != nullptr);
    check(qtw->count() == 2, "a git-only mod shows 2 tabs (Git, '+')");
    check(qtw->tabText(0) == QLatin1String("Git"),
          "a git-only mod is the Git tab, not Manual");

    bool banner = false;
    for (auto *p : tab.findChildren<ui::GitSourcePanel *>()) {
      for (auto *l : p->findChildren<QLabel *>()) {
        if (l->isVisibleTo(p) &&
            l->text().contains(QLatin1String("downloaded from a different source")))
          banner = true;
      }
    }
    check(!banner, "a git-only mod gets no coexistence warning");
  }
}

// The Git tab wears the branded badge when the remote's host is known and the
// generic one otherwise, and every other source tab keeps its own badge.
//
// The defect this guards: add_tab_with_icon() took a "vendor key" and fed
// whatever it was handed back through engine::vendor_icon_key(). The git call
// site passed an ALREADY-final key from icon_key_for() - "github" or "git" -
// and vendor_icon_key() knows neither, so it returned "" and the tab was added
// with no icon at all. The other sources pass a source_type, which that map
// does know, which is why only the Git tab was bare.
//
// Asserting the resolved KEY proves nothing here: the broken code produced a
// correct "github" key and then discarded it. These assertions read the icon
// the QTabBar actually holds, and compare it against the exact asset the key
// names, so a tab can be caught wearing the wrong badge as well as none.
//
// Hermetic: IconManager is pointed at a synthetic resources tree holding one
// flat-colour image per key, so no bundled asset, icon pack or desktop icon
// theme takes part. Offscreen; throwaway XDG_CONFIG_HOME.
TEST_CASE("source tab icons", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_source_tab_icons";
  std::filesystem::remove_all(root);
  const std::filesystem::path cfg = root / "config";
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();  // process-lifetime QApplication (see above)
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path mods_dir = root / "instances" / "Test" / "mods";
  std::filesystem::create_directories(mods_dir);

  // 8x8 solid-colour PNGs; the colour IS the identity of the key, so two
  // tabs that must differ cannot pass by both resolving to the same file.
  static const char *kRedPng =
      "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP4z8CAFWEXHbQSACj/"
      "P8Fu7N9hAAAAAElFTkSuQmCC";
  static const char *kGreenPng =
      "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEElEQVR4nGNgOMGAHQ0tCQCJ5TIBaf"
      "w8vQAAAABJRU5ErkJggg==";
  static const char *kBluePng =
      "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEElEQVR4nGNgYPiPAw0pCQCpcD/BFM"
      "rqcwAAAABJRU5ErkJggg==";
  const QColor kGithub(255, 0, 0), kGit(0, 200, 0), kNexus(0, 0, 255);

  // discover_packs() derives the resources dir from the app dir's PARENT, so
  // <root>/app resolves <root>/resources/icons/vendor/<key>.*. The swatches are
  // named .png, not .ico: Qt picks a decoder by suffix, and a PNG wearing an
  // .ico name loads as nothing. What is under test is the key -> file ->
  // QTabBar chain, not Qt's ICO reader.
  const std::filesystem::path vendor = root / "resources" / "icons" / "vendor";
  std::filesystem::create_directories(vendor);
  auto write_swatch = [&](const char *key, const char *b64) {
    std::ofstream out(vendor / (std::string(key) + ".png"), std::ios::binary);
    const auto bytes = QByteArray::fromBase64(b64);
    out.write(bytes.constData(), bytes.size());
  };
  write_swatch("github", kRedPng);
  write_swatch("git", kGreenPng);
  write_swatch("nexusmods", kBluePng);
  engine::IconManager::instance().discover_packs(root / "app");

  // An unmapped source leaves the final key empty; the tab must then carry no
  // icon rather than a stray one.
  check(engine::IconManager::instance().resolve_icon(QString()).isNull(),
        "an empty icon key resolves to no icon at all");
  // The fixture itself, or nothing below it means anything.
  check(!engine::IconManager::instance().resolve_icon(QLatin1String("github")).isNull(),
        "the synthetic vendor tree resolves at all");
  // The colour a tab wears. An unpainted tab yields an invalid colour, which is
  // what keeps "no icon" distinguishable from "the wrong icon".
  auto tab_colour = [](QTabWidget *qtw, int index) {
    const QPixmap pm = qtw->tabBar()->tabIcon(index).pixmap(8, 8);
    return pm.isNull() ? QColor() : pm.toImage().pixelColor(0, 0);
  };

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // --- Git + Nexus on github.com: the branded badge, and the Nexus tab keeps
  //     its own. ---
  {
    const std::string id                = "IconNexusMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = https://github.com/user/repo.git\n";
    }

    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "nexus");
    meta.set("GameModManager", "source_id", "42");
    meta.set("Nexusmods", "modid", "42");
    meta.save(mods_dir, id);

    ui::ModInfoData data = make_data(id, nullptr, mods_dir);
    data.mod_dir         = QDir(QString::fromStdString(mod_dir.string()));
    data.is_git          = true;
    data.git_remote_url  = QStringLiteral("https://github.com/user/repo.git");

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    REQUIRE(qtw != nullptr);
    check(qtw->count() == 3, "git + nexus shows 3 tabs (Nexus, Git, '+')");
    check(qtw->tabText(1) == QLatin1String("Git"), "tab 1 is the Git tab");
    check(tab_colour(qtw, 1) == kGithub,
          "a github.com remote paints the branded github badge on the Git tab");
    check(tab_colour(qtw, 0) == kNexus,
          "the Nexus tab still paints its own vendor badge");
    check(!qtw->tabIcon(1).isNull(),
          "the Git tab carries a real QIcon, not a null one that paints nothing");
  }

  // --- Git only on a host with no badge of its own: the generic one, still an
  //     icon. bitbucket.org is such a host; gitlab.com and a gitea.* host are
  //     not, and each resolves to a badge of its own. ---
  {
    const std::string id                = "IconGitOnlyMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = git@bitbucket.org:user/repo.git\n";
    }

    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "manual");
    meta.save(mods_dir, id);

    ui::ModInfoData data = make_manual_data(id, mods_dir);
    data.mod_dir         = QDir(QString::fromStdString(mod_dir.string()));
    data.is_git          = true;
    data.git_remote_url  = QStringLiteral("git@bitbucket.org:user/repo.git");

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    REQUIRE(qtw != nullptr);
    check(qtw->count() == 2, "a git-only mod shows 2 tabs (Git, '+')");
    check(qtw->tabText(0) == QLatin1String("Git"), "tab 0 is the Git tab");
    check(tab_colour(qtw, 0) == kGit,
          "a non-github host falls back to the generic git badge, still an icon");
    check(tab_colour(qtw, 0) != kGithub,
          "branded and generic git badges are distinguishable from each other");
  }
}

// A Steam-sourced mod whose description is long enough to take the ASYNC
// bbcode path, driven through ModInfoDialog - the surface the app actually
// opens the Source tab on. The dialog builds all nine tabs, restores the
// last-used tab from Settings (so Source is selected the moment the dialog
// opens) and eagerly calls set_mod() on the tabs it can enable-disable.
//
// The description has to be over set_bbcode_html_async()'s 1 KiB threshold and
// multi-core has to be on, or the parse runs synchronously on the UI thread
// and there is nothing in flight. Both are the app's defaults, which is why
// this only ever showed up on a real mod with a real description.
TEST_CASE("source tab steam mod survives a rebuild mid-parse", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_source_tab_steam";
  std::filesystem::remove_all(root);
  const std::filesystem::path cfg = root / "config";
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();  // process-lifetime QApplication (see above)
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path mods_dir = root / "instances" / "Test" / "mods";
  const std::filesystem::path mod_dir  = mods_dir / "SteamMod";
  std::filesystem::create_directories(mod_dir);

  // Real Steam Workshop metadata, with a description comfortably past the
  // async threshold.
  {
    std::string desc = "[b]A mod.[/b]\n";
    while (desc.size() < 4096)
      desc += "[quote]some longer workshop chatter[/quote] and a "
              "https://example.com/link\n";
    std::ofstream xml(mod_dir / "metadata.xml");
    xml << "<metadata>\n  <version>2.1</version>\n  <description>" << desc
        << "</description>\n</metadata>\n";
  }

  engine::ModMeta meta;
  meta.set("General", "version", "2.1");
  meta.set("GameModManager", "source_type", "steam");
  meta.set("GameModManager", "source_id", "123456789");
  meta.set("SteamWorkshop", "workshop_id", "123456789");
  meta.set("SteamWorkshop", "description", "workshop body");
  meta.save(mods_dir, "SteamMod");

  ui::ModInfoData data;
  data.id          = QStringLiteral("SteamMod");
  data.name        = QStringLiteral("SteamMod");
  data.version     = QStringLiteral("2.1");
  data.source_type = QStringLiteral("steam");
  data.source_id   = QStringLiteral("123456789");
  data.mod_dir     = QDir(QString::fromStdString(mod_dir.string()));
  data.delete_mod  = [] {
    return false;
  };
  data.load_meta = [mods_dir] {
    return engine::ModMeta::load(mods_dir, "SteamMod");
  };
  data.save_meta = [mods_dir](const engine::ModMeta &m) {
    return m.save(mods_dir, "SteamMod");
  };

  // Opening Mod Info with Source already selected: the dialog constructs every
  // tab, then activates Source, which runs set_mod() and
  // first_activation() over it back to back.
  ui::ModInfoDialog dialog(data, {{QStringLiteral("SteamMod"), true}},
                           ui::ModInfoTabId::Source, nullptr);
  dialog.show();
  for (int i = 0; i < 40; ++i) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(25);
  }

  check(dialog.current_mod_id() == QLatin1String("SteamMod"),
        "the dialog opened on the Steam mod");

  auto *tab = dialog.findChild<ui::SourceTab *>();
  REQUIRE(tab != nullptr);
  ui::SteamSourcePanel *steam = nullptr;
  for (auto *p : tab->findChildren<ui::SteamSourcePanel *>())
    steam = p;
  check(steam != nullptr, "the Steam panel survives the dialog's rebuild");
  check(steam && steam->has_data(), "the rebuilt Steam panel still reports data");

  // The same shape for the other sources, because the crash was never Steam
  // specific: it was whatever built the first QWebEngineView. A regression
  // here would hit all of them.
  struct {
    const char *type;
    const char *id;
  } kOthers[] = {
      {"nexus", "555"},
      {"loverslab", "666"},
      {"modpub", "777"},
  };
  for (const auto &other : kOthers) {
    const std::string id            = std::string(other.type) + "Mod";
    const std::filesystem::path dir = mods_dir / id;
    std::filesystem::create_directories(dir);
    std::ofstream xml(dir / "metadata.xml");
    std::string desc = "[b]Body.[/b]\n";
    while (desc.size() < 4096)
      desc += "[quote]more text[/quote] and https://example.com/x\n";
    xml << "<metadata>\n  <version>1.4</version>\n  <description>" << desc
        << "</description>\n</metadata>\n";
    xml.close();

    engine::ModMeta m;
    m.set("General", "version", "1.4");
    m.set("GameModManager", "source_type", other.type);
    m.set("GameModManager", "source_id", other.id);
    if (std::string(other.type) == "nexus") {
      m.set("Nexusmods", "modid", other.id);
    } else if (std::string(other.type) == "loverslab") {
      m.set("LoversLab", "fileid", other.id);
    } else {
      m.set("ModPub", "mod_id", other.id);
    }
    m.save(mods_dir, id);

    ui::ModInfoData d;
    d.id          = QString::fromStdString(id);
    d.name        = QString::fromStdString(id);
    d.version     = QStringLiteral("1.4");
    d.source_type = QString::fromLatin1(other.type);
    d.source_id   = QString::fromLatin1(other.id);
    d.mod_dir     = QDir(QString::fromStdString(dir.string()));
    d.delete_mod  = [] {
      return false;
    };
    d.load_meta = [mods_dir, id] {
      return engine::ModMeta::load(mods_dir, id);
    };
    d.save_meta = [mods_dir, id](const engine::ModMeta &mm) {
      return mm.save(mods_dir, id);
    };

    ui::ModInfoDialog other_dlg(d, {{d.id, true}}, ui::ModInfoTabId::Source, nullptr);
    other_dlg.show();
    for (int i = 0; i < 10; ++i) {
      QApplication::processEvents(QEventLoop::AllEvents, 50);
      QThread::msleep(25);
    }
    check(other_dlg.current_mod_id() == d.id,
          std::string(other.type).append(" mod opens in Mod Info").c_str());
  }
}

// The containment that stops a Vulkan overlay from taking the process down has
// to be in place before Chromium starts, and Chromium starts on the first
// QWebEngineView - which is whichever description the user happens to open
// first, not app launch, because profile setup was deferred to first use. So it
// is asserted here on the path that builds a view: the one factory all four
// source panels go through. Red before the loader was pointed at no driver,
// because nothing set VK_DRIVER_FILES.
TEST_CASE("chromium configuration is applied before the first web view", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  shared_app();

  // Read by WebEngine when it starts Chromium, i.e. immediately below.
  check(qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS"),
        "the Chromium switches are in the environment before any view exists");
#ifdef Q_OS_LINUX
  check(qgetenv("VK_DRIVER_FILES") == QByteArray("/dev/null"),
        "the Vulkan loader is pointed at no driver before any view exists");
#endif

  auto *view = ui::create_description_renderer();
  REQUIRE(view != nullptr);
  view->set_description(QStringLiteral("<p>a mod description</p>"));
  check(view->current_description() == QStringLiteral("<p>a mod description</p>"),
        "a description renders through a view built on that path");
  delete view;
}

// The per-source actions: a mod can carry more than one source at a time (a
// Nexus id AND a Steam workshop id, plus a .git in the folder), and exactly one
// of them is primary. The primary is recorded in the source's OWN sidecar
// section, so setting it needs no new storage format and survives a restart -
// these cases re-load the meta from disk after every write rather than reading
// the in-memory copy back.
//
// What a single-source mod does is the load-bearing half: it is primary without
// the user clicking anything, and a mod with no source at all has none.
TEST_CASE("source tab primary source", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_source_tab_primary";
  std::filesystem::remove_all(root);
  const std::filesystem::path cfg = root / "config";
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path mods_dir = root / "instances" / "Test" / "mods";
  std::filesystem::create_directories(mods_dir);

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // --- A mod with exactly one source is primary without being told. ---
  {
    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "nexus");
    meta.set("GameModManager", "source_id", "12345");
    meta.set("Nexusmods", "modid", "12345");
    meta.save(mods_dir, "SingleMod");

    auto data        = make_manual_data("SingleMod", mods_dir);
    data.source_type = QStringLiteral("nexus");
    data.source_id   = QStringLiteral("12345");

    check(ui::primary_source(data) == QLatin1String("nexus"),
          "a one-source mod is implicitly primary, with nothing recorded");

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    QApplication::processEvents();

    auto *qtw = tab.findChild<QTabWidget *>();
    REQUIRE(qtw != nullptr);
    check(qtw->count() == 2, "single source: 2 tabs (Nexus, '+')");
    check(qtw->tabText(0) == QLatin1String("Test Nexus"),
          "the only source carries no primary marker: there is nothing to pick");

    // Nothing to make primary either, so the flag must not appear - if it did,
    // a later source could demote a source the user never chose.
    check(engine::ModMeta::load(mods_dir, "SingleMod")
              .get("Nexusmods", "primary")
              .empty(),
          "an implicit primary writes no flag to the sidecar");
  }

  // --- Two sources: setting one primary demotes the other and persists. ---
  {
    const std::string id                = "TwoMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir);
    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "nexus");
    meta.set("GameModManager", "source_id", "111");
    meta.set("Nexusmods", "modid", "111");
    meta.set("SteamWorkshop", "workshop_id", "222");
    meta.save(mods_dir, id);

    auto data        = make_manual_data(id, mods_dir);
    data.source_type = QStringLiteral("nexus");
    data.source_id   = QStringLiteral("111");
    data.mod_dir     = QDir(QString::fromStdString(mod_dir.string()));

    // Nexus is first in the fixed order, so it leads until told otherwise.
    check(ui::primary_source(data) == QLatin1String("nexus"),
          "with two sources and no flag, the first one is primary");

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    QApplication::processEvents();
    auto *qtw = tab.findChild<QTabWidget *>();
    REQUIRE(qtw != nullptr);
    check(qtw->count() == 3, "two sources: 3 tabs (Nexus, Steam, '+')");
    check(qtw->tabText(0) == QLatin1String("Test Nexus (primary)"),
          "the primary source is visibly marked on its tab");
    // No Steam provider is registered in this fixture, so the Steam tab falls
    // back to its source_type for a title - the same fallback the app shows
    // for any provider that is not registered.
    check(qtw->tabText(1) == QLatin1String("steam"),
          "the non-primary source is not marked");

    // Steam is made primary first, so the demotion below removes a flag that
    // is actually there. Asserting it against a section that was never flagged
    // would pass even if the demotion did nothing.
    check(ui::apply_primary_source(data, QStringLiteral("steam")),
          "a primary can be set");
    check(engine::ModMeta::load(mods_dir, id).get("SteamWorkshop", "primary") == "true",
          "the flag is recorded in the new primary's own section");
    // The user right-clicks Nexus and picks "Set as primary".
    check(ui::apply_primary_source(data, QStringLiteral("nexus")),
          "the primary source can be moved");
    check(!ui::apply_primary_source(data, QStringLiteral("loverslab")),
          "a source with no section cannot be made primary");

    // Re-read from disk: this is the restart path.
    engine::ModMeta after = engine::ModMeta::load(mods_dir, id);
    check(after.get("Nexusmods", "primary") == "true",
          "the new primary is recorded in its own section");
    check(after.get("SteamWorkshop", "primary").empty(),
          "the previous primary is demoted in the same write");
    check(ui::primary_source(data) == QLatin1String("nexus"),
          "the new primary survives the reload and leads the tab order");

    // Never two primaries, read back out of the file rather than the model.
    int flagged = 0;
    for (const auto &section : after.sections()) {
      if (after.get(section, "primary") == "true")
        ++flagged;
    }
    check(flagged == 1, "exactly one section is flagged primary");

    ui::SourceTab reopened;
    reopened.set_current(data);
    reopened.set_mod(data);
    QApplication::processEvents();
    auto *qtw2 = reopened.findChild<QTabWidget *>();
    REQUIRE(qtw2 != nullptr);
    check(qtw2->tabText(0) == QLatin1String("Test Nexus (primary)"),
          "the tab bar leads with the primary the user picked");
    check(qtw2->tabText(1) == QLatin1String("steam"),
          "the demoted source keeps its own tab, unmarked");
  }

  // --- Git counts as a source: a mod with .git and a Nexus id has two, so
  //     Nexus is primary by position and the flag can move it. ---
  {
    const std::string id                = "GitNexusPrimary";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = https://github.com/user/repo.git\n";
    }
    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "manual");
    meta.set("Nexusmods", "modid", "333");
    meta.save(mods_dir, id);

    auto data    = make_manual_data(id, mods_dir);
    data.mod_dir = QDir(QString::fromStdString(mod_dir.string()));

    check(ui::primary_source(data) == QLatin1String("nexus"),
          "a .git in the folder makes Git a source, not the only one");
    check(ui::apply_primary_source(data, QStringLiteral("git")),
          "git can be made the primary source");
    engine::ModMeta after = engine::ModMeta::load(mods_dir, id);
    check(after.get("Git", "primary") == "true", "the flag lands in [Git]");
    check(after.get("Nexusmods", "primary").empty(), "[Nexusmods] no longer flagged");
    check(ui::primary_source(data) == QLatin1String("git"),
          "git leads the tab order once it is primary");
  }
}

// Deleting a source detaches it from the mod and does NOTHING else: the mod
// stays installed, every file it has stays on disk, and the other sources are
// untouched. That is the whole risk of the action, so it is what the case
// checks - the files, not just the meta.
//
// Detaching the LAST source is allowed and leaves a mod that is a manual
// install: a real state with a visible consequence, not an error.
TEST_CASE("source tab delete source", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_source_tab_delete";
  std::filesystem::remove_all(root);
  const std::filesystem::path cfg = root / "config";
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path mods_dir = root / "instances" / "Test" / "mods";
  std::filesystem::create_directories(mods_dir);

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  const std::string id                = "DeleteMod";
  const std::filesystem::path mod_dir = mods_dir / id;
  std::filesystem::create_directories(mod_dir / "meshes");
  {
    // The installed file the mod is made of. Detaching a source must leave it
    // exactly here.
    std::ofstream mesh(mod_dir / "meshes" / "thing.nif");
    mesh << "<nif>\n";
  }

  engine::ModMeta meta;
  meta.set("General", "version", "1.0");
  meta.set("General", "modid", "555");
  meta.set("GameModManager", "source_type", "nexus");
  meta.set("GameModManager", "source_id", "555");
  meta.set("Nexusmods", "modid", "555");
  meta.set("Nexusmods", "nexusdescription", "from nexus");
  meta.set("SteamWorkshop", "workshop_id", "666");
  meta.set("SteamWorkshop", "description", "from workshop");
  meta.save(mods_dir, id);

  auto data        = make_manual_data(id, mods_dir);
  data.source_type = QStringLiteral("nexus");
  data.source_id   = QStringLiteral("555");
  data.mod_dir     = QDir(QString::fromStdString(mod_dir.string()));

  check(ui::detach_source(data, QStringLiteral("nexus")),
        "an attached source can be detached");
  check(!ui::detach_source(data, QStringLiteral("loverslab")),
        "a source the mod never had cannot be detached");

  engine::ModMeta after = engine::ModMeta::load(mods_dir, id);
  // Only the Nexus record is gone. The Steam source was never the target.
  check(!after.has_section("Nexusmods"), "the detached source's section is gone");
  check(after.has_section("SteamWorkshop"), "the other source's section is untouched");
  check(after.get("SteamWorkshop", "description") == "from workshop",
        "the other source's stored metadata survives");
  check(after.get("SteamWorkshop", "workshop_id") == "666",
        "the other source's id survives");
  // The mod must not keep claiming the id it gave up.
  check(after.get("GameModManager", "source_type") == "manual",
        "the declared attribution no longer names the detached source");
  check(after.get("General", "modid").empty(),
        "the Nexus id namespace in [General] is cleared with it");
  check(after.get("General", "version") == "1.0", "unrelated metadata is untouched");

  // The point of the action: the mod is NOT uninstalled.
  check(std::filesystem::exists(mod_dir / "meshes" / "thing.nif"),
        "the mod's installed file is still on disk");
  check(std::filesystem::exists(mod_dir / "meta.ini"),
        "the mod is still installed (its sidecar is there)");
  check(std::filesystem::is_directory(mod_dir), "the mod folder itself is untouched");

  // What the tab looks like afterwards: the remaining source, no placeholder.
  auto steam_data        = data;
  steam_data.source_type = QStringLiteral("manual");
  ui::SourceTab tab;
  tab.set_current(steam_data);
  tab.set_mod(steam_data);
  QApplication::processEvents();
  auto *qtw = tab.findChild<QTabWidget *>();
  REQUIRE(qtw != nullptr);
  check(qtw->count() == 2, "after deleting Nexus: 2 tabs (Steam, '+')");
  // No Steam provider is registered in this fixture, so the tab falls back to
  // its source_type for a title.
  check(qtw->tabText(0) == QLatin1String("steam"),
        "the surviving source is the only tab, and unmarked: one source is primary");

  // Deleting the last one is allowed. The mod becomes a manual install.
  check(ui::detach_source(steam_data, QStringLiteral("steam")),
        "the last remaining source can be detached too");
  engine::ModMeta bare = engine::ModMeta::load(mods_dir, id);
  check(!bare.has_section("SteamWorkshop"), "no source section is left");
  check(bare.get("GameModManager", "source_type") == "manual",
        "a mod with no source is a manual install");
  check(std::filesystem::exists(mod_dir / "meshes" / "thing.nif"),
        "the mod's files survive losing every source");

  auto bare_data = steam_data;
  ui::SourceTab bare_tab;
  bare_tab.set_current(bare_data);
  bare_tab.set_mod(bare_data);
  QApplication::processEvents();
  auto *qtw2 = bare_tab.findChild<QTabWidget *>();
  REQUIRE(qtw2 != nullptr);
  check(qtw2->count() == 2, "no sources: 2 tabs (Manual placeholder, '+')");
  check(qtw2->tabText(0) == QLatin1String("Manual"),
        "the mod with no sources shows the Manual placeholder");
}

// A Git source's description is the README.md in the mod root, rendered through
// the same description renderer the download-source panels use. 100% of the time
// for git: there is no fetch and no cached copy, the file is read on every
// populate. With no README there is no description and the panel says so rather
// than inventing one.
TEST_CASE("git source renders its README as the description", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path root = "/tmp/gmm_source_tab_readme";
  std::filesystem::remove_all(root);
  const std::filesystem::path cfg = root / "config";
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  shared_app();
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path mods_dir = root / "instances" / "Test" / "mods";
  std::filesystem::create_directories(mods_dir);

  engine::Source::Registry::instance().register_provider(
      std::make_unique<FakeNexusProvider>());

  // The description text handed to the renderer, whatever the backend turned it
  // into. Empty means the README never reached the description path.
  auto shown = [](ui::GitSourcePanel &panel) {
    for (auto *renderer : panel.findChildren<ui::DescriptionRenderer *>())
      return renderer->current_description();
    return QString();
  };
  auto says_no_readme = [](ui::GitSourcePanel &panel) {
    for (auto *label : panel.findChildren<QLabel *>()) {
      if (label->isVisibleTo(&panel) &&
          label->text().contains(QLatin1String("no README.md")))
        return true;
    }
    return false;
  };

  // --- A repo WITH a README: it is the description. ---
  {
    const std::string id                = "ReadmeMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = https://github.com/user/repo.git\n";
    }
    {
      std::ofstream readme(mod_dir / "README.md");
      readme << "# Fancy Mod\n\nInstalls a **fancy** thing.\n";
    }
    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "manual");
    meta.save(mods_dir, id);

    auto data    = make_manual_data(id, mods_dir);
    data.mod_dir = QDir(QString::fromStdString(mod_dir.string()));

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    ui::GitSourcePanel *panel = nullptr;
    for (auto *p : tab.findChildren<ui::GitSourcePanel *>())
      panel = p;
    REQUIRE(panel != nullptr);
    const QString html = shown(*panel);
    check(html.contains(QLatin1String("Fancy Mod")),
          "the README's heading is rendered as the description");
    check(html.contains(QLatin1String("fancy")),
          "the README's body text is rendered as the description");
    check(!says_no_readme(*panel),
          "a repo with a README is not told it has no description");

    // The README is re-read, not cached: a checkout that brings a new one shows
    // the new text.
    {
      std::ofstream readme(mod_dir / "README.md");
      readme << "# Renamed Mod\n";
    }
    panel->populate();
    check(shown(*panel).contains(QLatin1String("Renamed Mod")),
          "the README is read again on every populate, not cached");
  }

  // --- A repo with NO README: no description, and the panel says so. ---
  {
    const std::string id                = "NoReadmeMod";
    const std::filesystem::path mod_dir = mods_dir / id;
    std::filesystem::create_directories(mod_dir / ".git");
    {
      std::ofstream cfgout(mod_dir / ".git" / "config");
      cfgout << "[remote \"origin\"]\n\turl = https://github.com/user/repo.git\n";
    }
    engine::ModMeta meta;
    meta.set("General", "version", "1.0");
    meta.set("GameModManager", "source_type", "manual");
    meta.save(mods_dir, id);

    auto data    = make_manual_data(id, mods_dir);
    data.mod_dir = QDir(QString::fromStdString(mod_dir.string()));

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    ui::GitSourcePanel *panel = nullptr;
    for (auto *p : tab.findChildren<ui::GitSourcePanel *>())
      panel = p;
    REQUIRE(panel != nullptr);
    check(shown(*panel).isEmpty(),
          "a repo with no README has no description, rather than an invented one");
    check(says_no_readme(*panel), "and the panel says plainly that there is none");
  }
}
