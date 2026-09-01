// loverslab_source_panel_test.cpp - LoversLab Mod Info source panel + the
// dateModified / install-time out-of-date detection the user asked for.
//
// Three regressions are guarded here:
//
//   1) LoversLabSourcePanel::has_data() must mirror NexusSourcePanel's
//      Workspace-rvld fix: a mod with only a default "1.0" version (which
//      every install writes) must NOT report "LoversLab has data" - only
//      a mod that is actually LoversLab-sourced (data_.source_type ==
//      "loverslab") or already has a [LoversLab] section in its meta.
//
//   2) Refresh must run off the UI thread. Click Refresh -> the worker
//      thread (not qApp->thread()) runs the fetch and the result lands
//      in the mod's meta. Second click while the first is in flight must
//      coalesce into one follow-up fetch (no torn write, stale result
//      dropped).
//
//   3) The out-of-date label appears when dateModified > install_ts (at
//      date granularity). It stays hidden for: pre-feature mods
//      (no date_modified), unknown install times, and mod downloaded on
//      or after the page stamp.
//
// Hermetic: no network, no real config (XDG_CONFIG_HOME pointed at a
// throwaway dir).
#include "engine/mod/meta/mod_meta.h"
#include "engine/source/loverslab_provider.h"
#include "engine/source/source_provider.h"
#include "ui/modinfo/source_panels/loverslab_source_panel.h"
#include "ui/modinfo/source_tab.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSemaphore>
#include <QTextBrowser>
#include <QThread>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
} // namespace

// A LoversLab-typed provider with no network surface; find_provider() in
// source_tab.cpp matches its display name and source_type()=="loverslab"
// routes it to the LoversLabSourcePanel.
struct FakeLoversLabProvider : engine::SourceProvider {
  std::string source_type() const override { return "loverslab"; }
  bool fetch(const engine::Mod &, engine::PipelineContext &,
             const std::filesystem::path &) override {
    return false;
  }
  std::string display_name() const override { return "Test LoversLab"; }
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

static ui::ModInfoData
make_data(const std::string &id, const std::string &file_id,
          const std::string &page_url,
          std::function<engine::LoversLabModInfoResult()> fetch,
          const std::filesystem::path &meta_dir, qint64 installation_ts = 0) {
  ui::ModInfoData data;
  data.id = QString::fromStdString(id);
  data.name = QString::fromStdString(id);
  data.source_id = QString::fromStdString(file_id);
  data.source_page_url = QString::fromStdString(page_url);
  data.source_type = QStringLiteral("loverslab");
  data.supported_sources = QStringList{QStringLiteral("Test LoversLab")};
  data.fetch_loverslab_info = std::move(fetch);
  data.installation_ts = installation_ts;
  data.load_meta = [meta_dir, id] {
    return engine::ModMeta::load(meta_dir, id);
  };
  data.save_meta = [meta_dir, id](const engine::ModMeta &m) {
    return m.save(meta_dir, id);
  };
  return data;
}

TEST_CASE("loverslab source panel - refresh + out-of-date", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_ll_source_panel/config";
  std::filesystem::remove_all("/tmp/gmm_ll_source_panel");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path instance =
      "/tmp/gmm_ll_source_panel/instances/Test";
  const std::filesystem::path meta_dir = instance / "meta";
  std::filesystem::create_directories(meta_dir);

  engine::SourceRegistry::instance().register_provider(
      std::make_unique<FakeLoversLabProvider>());

  // ---- Scenario 1: a basic Refresh writes [LoversLab] keys from the
  // fetched result onto a worker thread. install_ts is set so the
  // out-of-date label can show.
  {
    std::atomic<bool> on_worker = false;
    ui::SourceTab tab;
    // 2025-06-01 UTC - BEFORE the page's dateModified below
    // (2025-06-05), so the page was updated AFTER the user
    // installed this mod -> out of date.
    const qint64 install_ts = 1748736000; // 2025-06-01 00:00 UTC
    auto data = make_data(
        "LLModA", "11488",
        "https://www.loverslab.com/files/file/11488-the-xims-magazine/",
        [&]() -> engine::LoversLabModInfoResult {
          on_worker = QThread::currentThread() != qApp->thread();
          engine::LoversLabModInfoResult r;
          r.available = true;
          r.name = "The Xims Magazine";
          r.version = "1.1";
          r.author = "INueve";
          r.category = "Objects";
          r.description = "Hi all, the refreshed mod.";
          r.date_modified = "2025-06-05T14:23:11";
          r.page_url =
              "https://www.loverslab.com/files/file/11488-the-xims-magazine/";
          return r;
        },
        meta_dir, install_ts);
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();

    auto *refresh = find_refresh(tab);
    check(refresh != nullptr, "LoversLab page has a Refresh button");
    refresh->click();

    const bool landed = wait_for([&] {
      return engine::ModMeta::load(meta_dir, "LLModA")
                 .get("LoversLab", "description") ==
             "Hi all, the refreshed mod.";
    });
    check(landed, "refresh result persisted to the mod's meta");
    check(on_worker, "fetch ran on the worker thread, not the UI thread");

    const auto meta_after = engine::ModMeta::load(meta_dir, "LLModA");
    check(meta_after.get("LoversLab", "fileid") == "11488",
          "file id persisted (writes happen when source_type==loverslab)");
    check(meta_after.get("General", "version") == "1.1",
          "version persisted in [General]");
    check(meta_after.get("LoversLab", "category") == "Objects",
          "category persisted");
    check(meta_after.get("LoversLab", "author") == "INueve",
          "author persisted");
    check(meta_after.get("LoversLab", "date_modified") == "2025-06-05T14:23:11",
          "date_modified persisted (out-of-date detection input)");

    // Out-of-date badge: 2025-06-01 install + 2025-06-05 page stamp
    // means the page was updated AFTER the user installed -> out of
    // date. The label sits inside the source tab widget (which is
    // not the active tab in a tabbed widget, so isVisible() is
    // false even though the panel has populated it). Match on the
    // text content, which update_out_of_date_label() sets.
    bool ood_visible = false;
    for (auto *lbl : tab.findChildren<QLabel *>()) {
      if (!lbl->text().isEmpty() &&
          lbl->text().contains(QStringLiteral("out of date"))) {
        ood_visible = true;
        break;
      }
    }
    check(ood_visible,
          "Out-of-date badge text set when page dateModified > install_ts");

    // Description shows the refreshed text.
    bool desc_shown = false;
    for (auto *tb : tab.findChildren<QTextBrowser *>())
      if (tb->toPlainText().contains("refreshed mod"))
        desc_shown = true;
    check(desc_shown, "description browser shows the fetched text");
  }

  // ---- Scenario 2: superseding refresh drops stale result.
  {
    QSemaphore gate(0);
    std::atomic<int> calls = 0;
    ui::SourceTab tab;
    auto data = make_data(
        "LLModB", "12345", "https://www.loverslab.com/files/file/12345-other/",
        [&]() -> engine::LoversLabModInfoResult {
          ++calls;
          if (calls == 1)
            gate.tryAcquire(1, 5000);
          engine::LoversLabModInfoResult r;
          r.available = true;
          r.description = (calls == 2) ? "second" : "first";
          return r;
        },
        meta_dir);
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();
    auto *refresh = find_refresh(tab);
    REQUIRE(refresh != nullptr);
    refresh->click();
    check(wait_for([&] { return calls == 1; }, 2000),
          "first fetch started on the worker");
    // Re-enable the disabled Refresh to drive the coalescing path.
    refresh->setEnabled(true);
    refresh->click();
    gate.release();
    const bool landed = wait_for([&] {
      return engine::ModMeta::load(meta_dir, "LLModB")
                 .get("LoversLab", "description") == "second";
    });
    check(landed, "second refresh supersedes the first (stale dropped)");
    check(calls == 2, "exactly two fetches");
  }

  // ---- Scenario 3: same-day install -> not flagged. Date granularity
  // means installing on the same day as the page stamp is not out of
  // date (the analysis: "normalize dateModified (date granularity)
  // vs downloadTime (unix) for comparison"). The install is at
  // 12:00 UTC on 2025-06-05; the page stamp is 2025-06-05 (no time
  // component) -> equal at date granularity -> not flagged.
  {
    engine::ModMeta m;
    m.set("LoversLab", "fileid", "99999");
    m.set("LoversLab", "date_modified", "2025-06-05");
    m.save(meta_dir, "LLModSameDay");

    // 2025-06-05 12:00 UTC.
    const qint64 same_day = 1749124800; // 2025-06-05 12:00 UTC
    auto data = make_data("LLModSameDay", "99999",
                          "https://www.loverslab.com/files/file/99999-x/",
                          nullptr, meta_dir, same_day);
    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();
    bool ood_visible = false;
    for (auto *lbl : tab.findChildren<QLabel *>()) {
      if (!lbl->text().isEmpty() &&
          lbl->text().contains(QStringLiteral("out of date"))) {
        ood_visible = true;
        break;
      }
    }
    check(!ood_visible,
          "same-day install vs page stamp: not flagged as out of date");
  }

  // ---- Scenario 4: missing date_modified (pre-feature mod) -> not
  // flagged as out of date. The user requirement: mods downloaded
  // before this feature lands should be "unknown", NOT out of date.
  {
    engine::ModMeta m;
    m.set("LoversLab", "fileid", "88888");
    // No date_modified key.
    m.save(meta_dir, "LLModPreFeature");

    auto data =
        make_data("LLModPreFeature", "88888",
                  "https://www.loverslab.com/files/file/88888-x/", nullptr,
                  meta_dir, 1748736000); // 2025-06-01 (pre-feature mod)
    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();
    bool ood_visible = false;
    for (auto *lbl : tab.findChildren<QLabel *>()) {
      if (!lbl->text().isEmpty() &&
          lbl->text().contains(QStringLiteral("out of date"))) {
        ood_visible = true;
        break;
      }
    }
    check(!ood_visible, "missing date_modified (pre-feature mod): not flagged");
  }

  // ---- Scenario 5: has_data() mirrors the Workspace-rvld guard - a
  // mod with only a default version (no [LoversLab] section, not
  // LoversLab-sourced) must NOT report LoversLab has_data.
  {
    engine::ModMeta m;
    m.set("General", "version", "1.0");
    m.set("GameModManager", "source_type", "manual");
    m.save(meta_dir, "ManualMod");

    ui::ModInfoData data;
    data.id = QStringLiteral("ManualMod");
    data.name = QStringLiteral("ManualMod");
    data.source_type = QStringLiteral("manual");
    data.source_id = QString();
    data.supported_sources = QStringList{QStringLiteral("Test LoversLab")};
    data.load_meta = [meta_dir] {
      return engine::ModMeta::load(meta_dir, "ManualMod");
    };
    data.save_meta = [meta_dir](const engine::ModMeta &m) {
      return m.save(meta_dir, "ManualMod");
    };

    ui::SourceTab tab;
    tab.set_current(data);
    tab.set_mod(data);
    tab.first_activation();
    QApplication::processEvents();
    ui::LoversLabSourcePanel *ll = nullptr;
    for (auto *p : tab.findChildren<ui::LoversLabSourcePanel *>())
      ll = p;
    check(ll && !ll->has_data(),
          "manual mod: LoversLab panel has_data()==false");
  }
}