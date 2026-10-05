// Offscreen GUI regression for the status bar's context label, its source
// readouts, and the left-hand label's single owner.
//
// Covers:
//   1. The persistent "game - instance - profile" label, composed for a loaded
//      instance and falling back per component, wired through the real
//      MainWindow so the value is the loaded one and not just a formatted
//      string.
//   2. Only a source that meters a request budget gets a label. A source with
//      no meter (LoversLab has no API and no cooldown we enforce) must show
//      nothing rather than a permanent "--". Steam IS metered - it has a
//      cooldown we enforce - so it keeps a readout, pinned here because
//      filtering by "has a meter" is exactly what could drop it.
//   3. The separator only exists with readouts on both sides of it; with
//      nothing loaded the bar used to end in a bare "|".
//   4. A transient status borrows the left-hand label and gives it back.
//
// Hermetic: QT_QPA_PLATFORM=offscreen via the test property, per-case unique
// /tmp scratch root plus throwaway XDG_CONFIG_HOME/XDG_DATA_HOME, an empty
// GameKnowledge. No network: the source providers are only asked for their
// in-process budget state. The Steam case uses the real provider the
// DownloadsController registers, so it asserts the shipped readout shape.
#include "engine/instance/instance.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/source/interface.h"
#include "engine/source/registry.h"
#include "engine/source/steam/provider.h"
#include "ui/main_window/main_window.h"
#include "ui/settings/settings.h"
#include "ui/widgets/status_bar.h"

#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QLabel>
#include <QThread>
#include <QTimer>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <system_error>
#include <unistd.h>

namespace {

// Qt allows one QApplication per process; ctest runs each TEST_CASE as its
// own process, but a shared instance keeps the file honest either way.
QApplication *ensure_app() {
  static int argc          = 1;
  static char app_name[]   = "status_bar_test";
  static char *argv[]      = {app_name, nullptr};
  if (auto *existing = qobject_cast<QApplication *>(QCoreApplication::instance()))
    return existing;
  return new QApplication(argc, argv);
}

struct ScopedScratch {
  std::filesystem::path root;
  std::filesystem::path instances;
  std::filesystem::path config;
  std::filesystem::path data;

  ScopedScratch() {
    static std::atomic<unsigned> seq{0};
    const auto pid = static_cast<unsigned long>(::getpid());
    root = std::filesystem::temp_directory_path() /
           ("gmm_status_bar_" + std::to_string(pid) + "_" +
            std::to_string(seq.fetch_add(1, std::memory_order_relaxed)));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // stale dir from a crashed run
    instances = root / "instances";
    config    = root / "config";
    data      = root / "data";
    std::filesystem::create_directories(config, ec);
    std::filesystem::create_directories(data, ec);
  }
  ~ScopedScratch() noexcept {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

// Env must be set before the QApplication exists; org/app names and the
// dont_ask seed after it (same order as the other MainWindow UI tests).
struct CaseSetup {
  ScopedScratch scratch;
  int test_argc      = 1;
  char test_argv0[5] = "test";
  char *test_argv[2] = {test_argv0, nullptr};
  QApplication *app = nullptr;

  CaseSetup() {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("XDG_CONFIG_HOME", QByteArray(scratch.config.string().c_str()));
    qputenv("XDG_DATA_HOME", QByteArray(scratch.data.string().c_str()));
    app = ensure_app();
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");
    Settings::instance().set_nxm_handler_check("dont_ask");
  }
};

// Runs the deleteLater()s set_sources() leaves behind, so child-widget
// assertions see the settled widget tree instead of the pending deletions.
void settle_deferred_deletes() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// A source with a budget we meter, for the "has a meter" half of the rule.
// Named per case: the registry is a process singleton with no dedup, so two
// cases that both register a fake would otherwise race for the first match
// when the binary is run with every case in one process.
class MeteredFake : public engine::Source::Interface {
public:
  MeteredFake(std::string name, std::string readout)
      : name_(std::move(name)), readout_(std::move(readout)) {}
  std::string source_type() const override { return name_; }
  std::string display_name() const override { return name_; }
  bool fetch(const ::engine::Mod &, ::engine::PipelineContext &,
             const std::filesystem::path &) override {
    return false;
  }
  engine::Source::SourceRateLimit rate_limit_readout() const override {
    engine::Source::SourceRateLimit out;
    out.metered = true;
    out.readout = readout_;
    return out;
  }

private:
  std::string name_;
  std::string readout_;
};

// A source with no API and no cooldown we enforce - the LoversLab shape.
class UnmeteredFake : public engine::Source::Interface {
public:
  explicit UnmeteredFake(std::string name) : name_(std::move(name)) {}
  std::string source_type() const override { return name_; }
  std::string display_name() const override { return name_; }
  bool fetch(const ::engine::Mod &, ::engine::PipelineContext &,
             const std::filesystem::path &) override {
    return false;
  }

private:
  std::string name_;
};

// The separator is the bar's only divider. QLabel derives from QFrame, so
// this counts VLine frames specifically rather than every child frame.
int separators(const ui::StatusBar &bar) {
  int count = 0;
  for (auto *frame : bar.findChildren<QFrame *>()) {
    if (frame->frameShape() == QFrame::VLine)
      ++count;
  }
  return count;
}

QLabel *left_label(const ui::StatusBar &bar) {
  auto labels = bar.findChildren<QLabel *>();
  return labels.isEmpty() ? nullptr : labels.front();
}

// Blocks until the bar's left-hand label reads `text`, or the deadline passes.
// The sleep matters: spinning on processEvents alone starves the short timer
// this case drives, so the expiry never lands. Same shape as pump_until in
// game_path_banner_test.cpp.
bool wait_for_left_label(const ui::StatusBar &bar, const QString &text) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (left_label(bar)->text() != text && elapsed.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return left_label(bar)->text() == text;
}

}  // namespace

// 1 + the real wiring: after an instance loads, the far-left label names the
// game, the instance and the profile, in that order.
TEST_CASE("status bar context label names game, instance and profile", "[ui]") {
  CaseSetup setup;

  auto inst = engine::Instance::installed("TestGame", setup.scratch.instances);
  inst.info().game_id = "testgame";
  REQUIRE(inst.create_directories());
  REQUIRE(inst.write_toml());

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "mods_subpath", "Mods");
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", setup.scratch.root / "game",
                  inst.info().root);

  auto *bar = w.findChild<ui::StatusBar *>();
  REQUIRE(bar != nullptr);
  CHECK(bar->context_text() == "Test Game - TestGame - Default");
  // The label shows the resting text - no transient status is borrowing it.
  REQUIRE(left_label(*bar) != nullptr);
  CHECK(left_label(*bar)->text() == "Test Game - TestGame - Default");

  // A profile switch re-labels it: the value has to follow the identity, not
  // be frozen at whatever the instance load happened to set.
  std::error_code ec;
  std::filesystem::create_directories(inst.info().root / "profiles" / "Alternate", ec);
  w.set_game_info("testgame", "Test Game", "Alternate", setup.scratch.root / "game",
                  inst.info().root);
  CHECK(bar->context_text() == "Test Game - TestGame - Alternate");
}

// The fallbacks, per component: a game that is genuinely unknown, and an
// instance or profile that is genuinely absent.
TEST_CASE("context label falls back per missing component", "[ui]") {
  CHECK(ui::context_label_text("Skyrim Special Edition", "TestGame", "Default") ==
        "Skyrim Special Edition - TestGame - Default");
  CHECK(ui::context_label_text("", "TestGame", "Default") ==
        "Unknown game - TestGame - Default");
  CHECK(ui::context_label_text("Test Game", "", "Default") ==
        "Test Game - ? - Default");
  CHECK(ui::context_label_text("Test Game", "TestGame", "") ==
        "Test Game - TestGame - ?");
}

// 2: a source with a meter gets a label, one without gets nothing, and Steam -
// metered, because the cooldown is ours - keeps its readout.
TEST_CASE("status bar labels only sources that meter a budget", "[ui]") {
  CaseSetup setup;

  auto &registry = engine::Source::Registry::instance();
  registry.register_provider(std::make_unique<MeteredFake>("MeteredFake", "7/9"));
  registry.register_provider(std::make_unique<UnmeteredFake>("UnmeteredFake"));
  // The real provider the app registers at startup, so the Steam assertion
  // covers the shipped readout rather than a stand-in.
  registry.register_provider(std::make_unique<engine::Source::Steam::Provider>(
      (setup.scratch.root / "workshop_cache.db").string(), 60));

  ui::StatusBar bar;
  bar.set_sources({"MeteredFake", "UnmeteredFake", "Steam"});

  // A metered source reads its real budget.
  REQUIRE(bar.source_names().contains("MeteredFake"));
  CHECK(bar.source_text("MeteredFake") == "MeteredFake: 7/9");

  // A source with no meter has no label at all - not a label reading "--".
  CHECK_FALSE(bar.source_names().contains("UnmeteredFake"));
  CHECK(bar.source_text("UnmeteredFake").isEmpty());

  // Steam has the cooldown we enforce, so it keeps a counter. Nothing has
  // fetched from it yet, so the honest reading is 0 of the 60/hour limit.
  REQUIRE(bar.source_names().contains("Steam"));
  CHECK(bar.source_text("Steam") == "Steam: 0/60");
}

// Settings > Sources > "Hide API Request Counter" (default off). The source
// readouts ARE the per-source request counters, so the setting drops the whole
// group - and the divider with it, or the bar would end in a bare "|".
TEST_CASE("hide API request counter drops the source readouts", "[ui][settings]") {
  CaseSetup setup;

  auto &registry = engine::Source::Registry::instance();
  registry.register_provider(std::make_unique<MeteredFake>("CounterFake", "7/9"));
  registry.register_provider(std::make_unique<engine::Source::Steam::Provider>(
      (setup.scratch.root / "workshop_cache.db").string(), 60));

  auto &s = Settings::instance();
  CHECK_FALSE(s.hide_api_counter());  // the stored default

  ui::StatusBar bar;
  bar.set_sources({"CounterFake", "Steam"});
  REQUIRE(bar.source_names().contains("CounterFake"));
  CHECK(bar.source_text("CounterFake") == "CounterFake: 7/9");

  s.set_hide_api_counter(true);
  bar.set_sources({"CounterFake", "Steam"});
  CHECK(bar.source_names().isEmpty());
  CHECK(bar.source_text("CounterFake").isEmpty());
  CHECK(bar.source_text("Steam").isEmpty());

  s.set_hide_api_counter(false);
  bar.set_sources({"CounterFake", "Steam"});
  CHECK(bar.source_names().contains("CounterFake"));
}

// The game's real download_sources list: Skyrim's includes LoversLab, which
// has no meter, so it must not appear while Nexus Mods does.
TEST_CASE("a game listing an unmetered source shows no label for it", "[ui]") {
  CaseSetup setup;

  ui::MainWindow w;
  engine::GameKnowledge knowledge;
  // The knowledge key as the Skyrim game-support plugin hooks it.
  knowledge.set("testgame", "download_sources", "Nexus Mods,LoversLab");
  knowledge.set("testgame", "mods_subpath", "Mods");
  w.set_game_knowledge(&knowledge);
  w.show();
  w.set_game_info("testgame", "Test Game", "Default", setup.scratch.root / "game", {});

  auto *bar = w.findChild<ui::StatusBar *>();
  REQUIRE(bar != nullptr);

  // Nexus is metered (the API budget), LoversLab is not (no API, no cooldown
  // we enforce) - so exactly one label, and never a "LoversLab: --". Logged
  // out there is no Nexus budget to report yet, so its readout may be "--";
  // the point is that the label exists at all and LoversLab's does not.
  CHECK(bar->source_names() == QStringList{"Nexus Mods"});
  CHECK(bar->source_text("LoversLab").isEmpty());
  CHECK(bar->source_text("Nexus Mods").startsWith("Nexus Mods: "));
}

// 3: the separator divides the source readouts from the rest of the bar, so it
// is only there when a readout is.
TEST_CASE("status bar separator only exists with a readout on both sides", "[ui]") {
  CaseSetup setup;

  auto &registry = engine::Source::Registry::instance();
  registry.register_provider(std::make_unique<MeteredFake>("SeparatorMeter", "1/1"));
  registry.register_provider(std::make_unique<UnmeteredFake>("SeparatorUnmetered"));

  ui::StatusBar bar;

  // Nothing loaded: the bar used to end in a bare "|", because the constructor
  // added a separator that only set_sources() removed.
  CHECK(separators(bar) == 0);

  // An unmetered source is not content, so it does not earn a separator.
  bar.set_sources({"SeparatorUnmetered"});
  settle_deferred_deletes();
  CHECK(separators(bar) == 0);
  CHECK(bar.source_names().isEmpty());

  // A metered source puts content on the far side of the divider.
  bar.set_sources({"SeparatorMeter"});
  settle_deferred_deletes();
  CHECK(separators(bar) == 1);

  // Dropping back to no sources takes the divider with it.
  bar.set_sources({});
  settle_deferred_deletes();
  CHECK(separators(bar) == 0);
}

// 4: the context label owns the left-hand text; a transient status borrows it
// and gives it back.
TEST_CASE("transient status gives the left-hand label back to the context", "[ui]") {
  CaseSetup setup;

  ui::StatusBar bar;

  // Real 5 s in the app; shortened here so the restore can be observed without
  // making the suite wait. Set BEFORE the first set_status - a running timer
  // keeps its original deadline across setInterval, so shortening afterwards
  // would leave the first message up for the full 5 s.
  auto *timer = bar.findChild<QTimer *>("statusTimeout");
  REQUIRE(timer != nullptr);
  timer->setInterval(1);

  bar.set_context("Test Game - TestGame - Default");
  REQUIRE(left_label(bar) != nullptr);
  CHECK(left_label(bar)->text() == "Test Game - TestGame - Default");

  bar.set_status("Mod mirrored to instance");
  CHECK(left_label(bar)->text() == "Mod mirrored to instance");

  // The expiry is what restores it, not the next set_context().
  REQUIRE(wait_for_left_label(bar, "Test Game - TestGame - Default"));

  // A new instance's context does not jump the queue while a message is on
  // screen, and is what shows once that one expires.
  bar.set_status("Debug mode enabled");
  bar.set_context("Other Game - OtherInstance - Alt");
  CHECK(left_label(bar)->text() == "Debug mode enabled");
  REQUIRE(wait_for_left_label(bar, "Other Game - OtherInstance - Alt"));
}
