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
#include "engine/source/nexus_provider.h"
#include "engine/source/source_provider.h"
#include "ui/modinfo/source_tab.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSemaphore>
#include <QTextBrowser>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

// A Nexus-typed provider with no network surface; find_provider() in
// source_tab.cpp matches its display name and source_type()=="nexus" routes
// it to the full metadata form.
struct FakeNexusProvider : engine::SourceProvider {
    std::string source_type() const override { return "nexus"; }
    bool fetch(const engine::Mod&, engine::PipelineContext&,
               const std::filesystem::path&) override {
        return false;
    }
    std::string display_name() const override { return "Test Nexus"; }
};

static QPushButton* find_refresh(QWidget& w) {
    for (auto* b : w.findChildren<QPushButton*>())
        if (b->text() == QLatin1String("Refresh"))
            return b;
    return nullptr;
}

static bool wait_for(const std::function<bool()>& pred, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (pred()) return true;
        QThread::msleep(10);
    }
    return pred();
}

static ui::ModInfoData make_data(const std::string& id,
                                 std::function<engine::ModInfoResult()> fetch,
                                 const std::filesystem::path& meta_dir) {
    ui::ModInfoData data;
    data.id = QString::fromStdString(id);
    data.name = QString::fromStdString(id);
    data.source_id = QStringLiteral("42");
    data.nexus_domain = QStringLiteral("testgame");
    data.supported_sources = QStringList{QStringLiteral("Test Nexus")};
    data.fetch_nexus_info = std::move(fetch);
    data.load_meta = [meta_dir, id] {
        return engine::ModMeta::load(meta_dir, id);
    };
    data.save_meta = [meta_dir, id](const engine::ModMeta& m) {
        return m.save(meta_dir, id);
    };
    return data;
}

TEST_CASE("source tab", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_source_tab/config";
    std::filesystem::remove_all("/tmp/gmm_source_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const std::filesystem::path instance = "/tmp/gmm_source_tab/instances/Test";
    const std::filesystem::path meta_dir = instance / "meta";
    std::filesystem::create_directories(meta_dir);

    engine::SourceRegistry::instance().register_provider(
        std::make_unique<FakeNexusProvider>());

    // --- Scenario 1: a plain Refresh fetches off the main thread and lands. ---
    {
        std::atomic<int> calls = 0;
        std::atomic<bool> on_worker = false;
        ui::SourceTab tab;
        auto data1 = make_data("ModA",
                               [&]() -> engine::ModInfoResult {
                                   ++calls;
                                   on_worker =
                                       QThread::currentThread() != qApp->thread();
                                   engine::ModInfoResult r;
                                   r.available = true;
                                   r.name = "Fetched Mod";
                                   r.version = "2.0";
                                   r.newest_version = "2.1";
                                   r.category_id = "7";
                                   r.description = "Fetched description";
                                   return r;
                               },
                               meta_dir);
        tab.set_current(data1);
        tab.set_mod(data1);
        tab.first_activation();
        QApplication::processEvents();

        auto* refresh = find_refresh(tab);
        if (!refresh) {
            std::printf("DEBUG buttons:");
            for (auto* b : tab.findChildren<QPushButton*>())
                std::printf(" [%s]", qPrintable(b->text()));
            std::printf("\n");
            for (auto* lbl : tab.findChildren<QLabel*>())
                std::printf("DEBUG label: %s\n", qPrintable(lbl->text()));
        }
        check(refresh != nullptr, "Nexus page has a Refresh button");
        refresh->click();

        const bool landed = wait_for([&] {
            return engine::ModMeta::load(meta_dir, "ModA")
                       .get("Nexusmods", "nexusdescription") ==
                   "Fetched description";
        });
        check(landed, "refresh result persisted to the mod's meta");
        check(on_worker, "fetch ran on the worker thread, not the UI thread");
        check(calls == 1, "one refresh = exactly one fetch");

        auto* refresh2 = find_refresh(tab);
        check(refresh2 != nullptr && refresh2->isEnabled() &&
                  refresh2->text() == QLatin1String("Refresh"),
              "Refresh button re-enabled after the result lands");

        bool desc_shown = false;
        for (auto* tb : tab.findChildren<QTextBrowser*>())
            if (tb->toPlainText().contains("Fetched description")) desc_shown = true;
        check(desc_shown, "description browser shows the fetched text");

        bool ver_shown = false;
        for (auto* le : tab.findChildren<QLineEdit*>())
            if (le->text() == QStringLiteral("2.0")) ver_shown = true;
        check(ver_shown, "version field shows the fetched version");
    }

    // --- Scenario 2: a second Refresh while one is in flight supersedes it. ---
    {
        QSemaphore gate(0);
        std::atomic<int> calls = 0;
        std::atomic<bool> on_worker = false;
        ui::SourceTab tab;
        auto data2 = make_data("ModB",
                               [&]() -> engine::ModInfoResult {
                                   ++calls;
                                   on_worker =
                                       QThread::currentThread() != qApp->thread();
                                   if (calls == 1)
                                       gate.tryAcquire(1, 5000);  // park the worker
                                   engine::ModInfoResult r;
                                   r.available = true;
                                   r.description =
                                       (calls == 2) ? "second result"
                                                    : "first result";
                                   return r;
                               },
                               meta_dir);
        tab.set_current(data2);
        tab.set_mod(data2);
        tab.first_activation();
        QApplication::processEvents();

        auto* refresh = find_refresh(tab);
        REQUIRE(refresh != nullptr);

        // First click: gen 1, the worker parks inside the fetch.
        refresh->click();
        check(wait_for([&] { return calls == 1; }, 2000),
              "first fetch started and parked on the worker");

        // The in-flight fetch disables the button (real UI affordance).
        check(!refresh->isEnabled() &&
                  refresh->text() == QStringLiteral("Fetching…"),
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
            return engine::ModMeta::load(meta_dir, "ModB")
                       .get("Nexusmods", "nexusdescription") == "second result";
        });
        check(landed,
              "second refresh supersedes the first (stale result dropped)");
        check(on_worker, "superseded fetch also ran on the worker thread");
        check(calls == 2, "coalesced: exactly two fetches, no third");
        check(engine::ModMeta::load(meta_dir, "ModB")
                      .get("Nexusmods", "nexusdescription") ==
                  "second result",
              "meta holds only the newer result (no torn write)");
    }
}
