// Regression test for the Settings -> Sources tab.
//
// Verifies the nested per-provider sub-tabs and the MO2-parity Nexus panel:
// the five group boxes (Nexus Account / Statistics / Nexus Connection /
// Options / Servers), account + rate-limit labels showing "N/A" while
// disconnected, the log line, the Connect/Manual/Disconnect button enablement
// against the stored key, the inert category-mappings checkbox, the
// drag&drop server lists and their persistence back to the engine, and the
// manual-key dialog. The "Queue downloads (one at a time)" checkbox's
// tier-derived default (Regular/Supporter -> ON, Premium -> OFF, applied only
// while the user has never set the value) is pinned here too. Also drives the
// engine modules behind the panel: NexusServers (discovery, speed sampling,
// preferred-list persistence) and NexusAuth user-info roundtrip.
//
// Hermetic: no network calls are made (the Connect button is never clicked —
// validating the key would hit users/validate.json). XDG_CONFIG_HOME points
// at a throwaway dir so keyring/rate-limit/server JSON never touches the real
// user config.
#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings.h"
#include "ui/settings/source_pages.h"

#include "engine/nexus_auth.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/nexus_provider.h"
#include "engine/source/nexus_servers.h"
#include "engine/source/source_provider.h"
#include "engine/source/steam_workshop_provider.h"
#include "engine/theme/style_manager.h"
#include "engine/theme/theme_manager.h"

#ifdef GMM_PLATFORM_LINUX
#include "platform/linux/linux_platform.h"
#endif

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
}

// A provider with no UI page: its sub-tab must fall back to the
// "no configurable settings" hint instead of showing a panel.
struct FakeProvider : engine::SourceProvider {
    std::string source_type() const override { return "fakesrc"; }
    bool fetch(const engine::Mod&, engine::PipelineContext&,
               const std::filesystem::path&) override {
        return false;
    }
    std::string display_name() const override { return "Fake Source"; }
};

static QTabWidget* find_outer_tabs(QDialog& dlg) {
    for (auto* t : dlg.findChildren<QTabWidget*>())
        for (int i = 0; i < t->count(); ++i)
            if (t->tabText(i).contains("Sources", Qt::CaseInsensitive))
                return t;
    return nullptr;
}

static QWidget* find_page(QTabWidget* tabs, const char* needle) {
    for (int i = 0; tabs && i < tabs->count(); ++i)
        if (tabs->tabText(i).contains(needle, Qt::CaseInsensitive))
            return tabs->widget(i);
    return nullptr;
}

static QWidget* find_sub_tab(QTabWidget* inner, const char* title) {
    for (int i = 0; inner && i < inner->count(); ++i)
        if (inner->tabText(i) == QLatin1String(title))
            return inner->widget(i);
    return nullptr;
}

// Rebuild a fresh dialog and hand back its Nexus sub-tab page.
static QWidget* nexus_page_of(std::unique_ptr<SettingsDialog>& dlg) {
    auto* tabs = find_outer_tabs(*dlg);
    auto* sources = find_page(tabs, "Sources");
    auto* inner = sources ? sources->findChild<QTabWidget*>() : nullptr;
    return find_sub_tab(inner, "Nexus Mods");
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Keep QSettings/keyring/server writes fully out of the real config.
    const std::filesystem::path cfg = "/tmp/gmm_sources_tab/config";
    std::filesystem::remove_all("/tmp/gmm_sources_tab");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    const std::filesystem::path root = "/tmp/gmm_sources_tab/instances/Test";
    std::filesystem::create_directories(root);

    engine::ThemeManager tm;
    engine::StyleManager style(tm);
    engine::PluginLoader loader;

    auto& auth = engine::NexusAuth::instance();
    auth.clear_api_key();
    auth.clear_user_info();
    engine::NexusServers::instance().clear_all();

    engine::SourceRegistry::instance().register_provider(
        std::make_unique<engine::NexusProvider>());
    engine::SourceRegistry::instance().register_provider(
        std::make_unique<engine::SteamWorkshopProvider>(
            "/tmp/gmm_sources_tab/workshop.db"));
    engine::SourceRegistry::instance().register_provider(
        std::make_unique<FakeProvider>());

    auto make_dialog = [&] {
        auto dlg = std::make_unique<SettingsDialog>(&style, "breeze", root, &loader);
        dlg->show();
        app.processEvents();
        return dlg;
    };

    // --- Dialog #1: initial disconnected state. ---
    {
        auto dlg = make_dialog();
        auto* tabs = find_outer_tabs(*dlg);
        check(tabs != nullptr, "outer tab widget present");
        auto* sources = find_page(tabs, "Sources");
        check(sources != nullptr, "Sources tab present");

        auto* inner = sources ? sources->findChild<QTabWidget*>() : nullptr;
        check(inner != nullptr, "Sources tab nests a per-provider QTabWidget");
        if (!inner) return 1;

        check(inner->count() == 3, "one sub-tab per registered provider (3)");
        bool has_nexus = false, has_steam = false, has_fake = false;
        for (int i = 0; i < inner->count(); ++i) {
            const QString t = inner->tabText(i);
            if (t == "Nexus Mods") has_nexus = true;
            if (t == "Steam Workshop") has_steam = true;
            if (t == "Fake Source") has_fake = true;
        }
        check(has_nexus && has_steam && has_fake,
              "sub-tabs: Nexus Mods / Steam Workshop / Fake Source");

        auto* fake = find_sub_tab(inner, "Fake Source");
        bool fake_hint = false;
        if (fake) {
            if (auto* lbl = qobject_cast<QLabel*>(fake)) {
                fake_hint = lbl->text() == "This source has no configurable settings.";
            } else {
                for (auto* lbl : fake->findChildren<QLabel*>())
                    if (lbl->text() == "This source has no configurable settings.")
                        fake_hint = true;
            }
        }
        check(fake_hint, "provider without a page shows the no-settings hint");

        auto* steam = find_sub_tab(inner, "Steam Workshop");
        bool steam_group = false;
        if (steam)
            for (auto* gb : steam->findChildren<QGroupBox*>())
                if (gb->title() == "Steam Web API Rate Limit") steam_group = true;
        check(steam_group, "Steam page has the rate-limit group");

        auto* nexus = find_sub_tab(inner, "Nexus Mods");
        bool acc = false, stats = false, conn = false, opts = false, srv = false;
        if (nexus)
            for (auto* gb : nexus->findChildren<QGroupBox*>()) {
                if (gb->title() == "Nexus Account") acc = true;
                if (gb->title() == "Statistics") stats = true;
                if (gb->title() == "Nexus Connection") conn = true;
                if (gb->title() == "Options") opts = true;
                if (gb->title() == "Servers") srv = true;
            }
        check(acc && stats && conn && opts && srv,
              "Nexus page has all five MO2 boxes");

        auto* uid = nexus ? nexus->findChild<QLabel*>("nexusUserID") : nullptr;
        auto* nm = nexus ? nexus->findChild<QLabel*>("nexusName") : nullptr;
        auto* acc_lbl = nexus ? nexus->findChild<QLabel*>("nexusAccount") : nullptr;
        auto* daily = nexus ? nexus->findChild<QLabel*>("nexusDailyRequests") : nullptr;
        auto* hourly = nexus ? nexus->findChild<QLabel*>("nexusHourlyRequests") : nullptr;
        check(uid && nm && acc_lbl && daily && hourly,
              "account/statistics labels present");
        check(uid && uid->text() == "N/A" && nm && nm->text() == "N/A" &&
                  acc_lbl && acc_lbl->text() == "N/A" && daily &&
                  daily->text() == "N/A" && hourly && hourly->text() == "N/A",
              "account + statistics show N/A while disconnected");

        auto* log_list = nexus ? nexus->findChild<QListWidget*>("nexusLog") : nullptr;
        check(log_list && log_list->count() == 1 &&
                  log_list->item(0)->text() == "Not connected.",
              "log starts with a single 'Not connected.' line");

        auto* connect_btn =
            nexus ? nexus->findChild<QPushButton*>("nexusConnect") : nullptr;
        auto* manual_btn =
            nexus ? nexus->findChild<QPushButton*>("nexusManualKey") : nullptr;
        auto* disconnect_btn =
            nexus ? nexus->findChild<QPushButton*>("nexusDisconnect") : nullptr;
        check(connect_btn && manual_btn && disconnect_btn,
              "connection buttons present");
        check(connect_btn && !connect_btn->isEnabled() && manual_btn &&
                  manual_btn->isEnabled() && disconnect_btn &&
                  !disconnect_btn->isEnabled(),
              "Connect/Disconnect disabled without a stored key, Manual enabled");

#ifdef GMM_PLATFORM_LINUX
        const bool associate_expected = !engine::LinuxPlatform::is_nxm_handler_registered();
#else
        const bool associate_expected = false;
#endif
        auto* associate =
            nexus ? nexus->findChild<QPushButton*>("associateButton") : nullptr;
        check(associate != nullptr, "associate button present");
        check(associate && associate->isEnabled() == associate_expected,
              "associate button mirrors nxm-handler registration state");

        QCheckBox* endorse = nullptr;
        QCheckBox* track = nullptr;
        QCheckBox* cat = nullptr;
        QCheckBox* counter = nullptr;
        QCheckBox* queue = nullptr;
        if (nexus)
            for (auto* cb : nexus->findChildren<QCheckBox*>()) {
                if (cb->text() == "Endorsement Integration") endorse = cb;
                if (cb->text() == "Tracked Integration") track = cb;
                if (cb->text() == "Apply Nexus category mappings") cat = cb;
                if (cb->text() == "Hide API Request Counter") counter = cb;
                if (cb->text() == "Queue downloads (one at a time)") queue = cb;
            }
        check(endorse && track && cat && counter && queue,
              "all five option checkboxes present");
        check(endorse && endorse->isChecked() && track && track->isChecked() &&
                  counter && !counter->isChecked() && queue && queue->isChecked(),
              "options initial states (endorse + track + queue on, counter off)");
        check(cat && !cat->isEnabled() &&
                  cat->toolTip().contains("Work in progress"),
              "category-mappings box is inert (disabled + WIP tooltip)");

        auto& s = Settings::instance();
        check(s.endorsement_integration() && s.tracked_integration() &&
                  !s.hide_api_counter(),
              "settings defaults match the checkbox states");
        endorse->toggle();
        app.processEvents();
        check(!s.endorsement_integration(), "endorsement toggle persisted");
        endorse->toggle();
        app.processEvents();
        check(s.endorsement_integration(), "endorsement re-toggle persisted back");
        track->toggle();
        app.processEvents();
        check(!s.tracked_integration(), "tracked toggle persisted");
        track->toggle();
        app.processEvents();
        counter->toggle();
        app.processEvents();
        check(s.hide_api_counter(), "counter toggle persisted");
        counter->toggle();
        app.processEvents();
        queue->toggle();
        app.processEvents();
        check(!s.nexus_queue_downloads() && s.nexus_queue_downloads_set(),
              "queue toggle persisted off (and becomes an explicit choice)");
        queue->toggle();
        app.processEvents();
        check(s.nexus_queue_downloads(), "queue re-toggle persisted back on");
        cat->setChecked(true);
        app.processEvents();
        check(!s.category_mappings(),
              "disabled category-mappings box never writes a value");

        auto* known = nexus ? nexus->findChild<QListWidget*>("knownServersList") : nullptr;
        auto* preferred =
            nexus ? nexus->findChild<QListWidget*>("preferredServersList") : nullptr;
        check(known && preferred, "known + preferred server lists present");
        check(known && preferred &&
                  known->dragDropMode() == QAbstractItemView::DragDrop &&
                  preferred->dragDropMode() == QAbstractItemView::DragDrop &&
                  known->defaultDropAction() == Qt::MoveAction &&
                  preferred->defaultDropAction() == Qt::MoveAction,
              "server lists are drag&drop with Move action");
        check(known && known->count() == 0 && preferred && preferred->count() == 0,
              "server lists start empty");
    }

    // --- Engine server registry: discovery, CDN preference, speed samples. ---
    auto& servers = engine::NexusServers::instance();
    servers.clear_all();
    servers.record_discovered("TestMirror", false);
    servers.record_discovered("CDN", true);
    {
        bool known_has_test = false, pref_has_cdn = false;
        for (const auto& srv : servers.known())
            if (srv.name == "TestMirror") known_has_test = true;
        for (const auto& srv : servers.preferred())
            if (srv.name == "CDN") pref_has_cdn = true;
        check(known_has_test && pref_has_cdn,
              "discovered mirrors land in known/preferred (CDN starts preferred)");
    }
    servers.record_speed("TestMirror", 1048576);
    check(servers.known().size() == 1 &&
              servers.known()[0].average_speed() == 1048576,
          "speed samples averaged and attached to the server");

    // --- Dialog #2: populated lists + drag&drop persistence. ---
    {
        auto dlg = make_dialog();
        auto* nexus = nexus_page_of(dlg);
        auto* known = nexus ? nexus->findChild<QListWidget*>("knownServersList") : nullptr;
        auto* preferred =
            nexus ? nexus->findChild<QListWidget*>("preferredServersList") : nullptr;
        check(known && known->count() == 1 &&
                  known->item(0)->text().contains("TestMirror") &&
                  known->item(0)->text().contains("1.0 MB/s"),
              "known list shows the mirror with its average speed");
        check(preferred && preferred->count() == 1 &&
                  preferred->item(0)->text() == "CDN (automatic)",
              "preferred list shows the CDN mirror marked automatic");

        // Simulate a drag&drop insert into Preferred: the rowsInserted signal
        // must persist the new preference to the engine.
        auto* item = new QListWidgetItem("TestMirror");
        item->setData(Qt::UserRole, "TestMirror");
        preferred->addItem(item);
        app.processEvents();
        {
            const auto pref_v = servers.preferred();
            bool has_test = false;
            for (const auto& srv : pref_v)
                if (srv.name == "TestMirror") has_test = true;
            check(pref_v.size() == 2 && has_test,
                  "drag&drop insert persisted both servers to the preferred list");
        }
        // Drag CDN back out: it must demote to known.
        for (int i = 0; i < preferred->count(); ++i)
            if (preferred->item(i)->data(Qt::UserRole).toString() == "CDN")
                delete preferred->takeItem(i);
        app.processEvents();
        {
            const auto pref_v = servers.preferred();
            const auto known_v = servers.known();
            bool pref_test = false, known_cdn = false;
            for (const auto& srv : pref_v)
                if (srv.name == "TestMirror") pref_test = true;
            for (const auto& srv : known_v)
                if (srv.name == "CDN") known_cdn = true;
            check(pref_v.size() == 1 && pref_test && known_v.size() == 1 &&
                      known_cdn,
                  "removing a mirror from preferred demotes it to known");
        }
    }

    // --- Dialog #3: the reordered preference survives a reopen. ---
    {
        auto dlg = make_dialog();
        auto* nexus = nexus_page_of(dlg);
        auto* known = nexus ? nexus->findChild<QListWidget*>("knownServersList") : nullptr;
        auto* preferred =
            nexus ? nexus->findChild<QListWidget*>("preferredServersList") : nullptr;
        check(known && known->count() == 1 &&
                  known->item(0)->text() == "CDN (automatic)",
              "reopened dialog: CDN back in known, marked automatic");
        check(preferred && preferred->count() == 1 &&
                  preferred->item(0)->text().contains("TestMirror"),
              "reopened dialog: TestMirror remains preferred");
    }
    servers.clear_all();

    // --- NexusAuth user-info roundtrip + display (no network involved). ---
    check(!auth.has_user_info(), "no persisted user info after clear");
    engine::NexusUserInfo info;
    info.user_id = "12345";
    info.name = "TestUser";
    info.account_type = engine::NexusUserInfo::AccountType::Supporter;
    auth.set_user_info(info);
    check(auth.has_user_info(), "user info persisted");
    const auto got = auth.get_user_info();
    check(got.user_id == "12345" && got.name == "TestUser" &&
              got.account_type == engine::NexusUserInfo::AccountType::Supporter,
          "user info roundtrips through disk");
    {
        auto dlg = make_dialog();
        auto* nexus = nexus_page_of(dlg);
        auto* uid = nexus ? nexus->findChild<QLabel*>("nexusUserID") : nullptr;
        auto* nm = nexus ? nexus->findChild<QLabel*>("nexusName") : nullptr;
        auto* acc_lbl = nexus ? nexus->findChild<QLabel*>("nexusAccount") : nullptr;
        check(uid && uid->text() == "12345" && nm && nm->text() == "TestUser" &&
                  acc_lbl && acc_lbl->text() == "Supporter",
              "account labels show persisted user info");
    }
    auth.clear_user_info();
    check(!auth.has_user_info(), "user info cleared");

    // --- Tier-derived queue-downloads default (applied on login, only while
    // --- the user has never set the value explicitly).
    auto queue_box_of = [](QWidget* nexus) -> QCheckBox* {
        if (!nexus) return nullptr;
        for (auto* cb : nexus->findChildren<QCheckBox*>())
            if (cb->text() == "Queue downloads (one at a time)") return cb;
        return nullptr;
    };
    auto& s = Settings::instance();
    s.remove_nexus_queue_downloads();
    check(!s.nexus_queue_downloads_set(), "queue-downloads setting starts unset");

    // Premium login -> the checkbox defaults OFF while unset (Premium lifts
    // the ~1.5MB/s cap, so parallel Nexus downloads are useful).
    {
        engine::NexusUserInfo prem;
        prem.account_type = engine::NexusUserInfo::AccountType::Premium;
        auth.set_user_info(prem);
        auto dlg = make_dialog();
        auto* q = queue_box_of(nexus_page_of(dlg));
        check(q && !q->isChecked(),
              "Premium account: queueing defaults OFF while unset");
    }
    // Supporter keeps the free-account throttle -> ON.
    {
        engine::NexusUserInfo sup;
        sup.account_type = engine::NexusUserInfo::AccountType::Supporter;
        auth.set_user_info(sup);
        auto dlg = make_dialog();
        auto* q = queue_box_of(nexus_page_of(dlg));
        check(q && q->isChecked(),
              "Supporter account: queueing defaults ON while unset");
    }
    // Regular -> ON.
    {
        engine::NexusUserInfo reg;
        reg.account_type = engine::NexusUserInfo::AccountType::Regular;
        auth.set_user_info(reg);
        auto dlg = make_dialog();
        auto* q = queue_box_of(nexus_page_of(dlg));
        check(q && q->isChecked(),
              "Regular account: queueing defaults ON while unset");
    }
    // A manual choice survives later logins: neither the checkbox state nor
    // apply_nexus_queue_default() may override it.
    {
        s.set_nexus_queue_downloads(false);
        auto dlg = make_dialog();
        auto* q = queue_box_of(nexus_page_of(dlg));
        check(q && !q->isChecked(),
              "manual OFF choice shown even for a Supporter login");
        ui::apply_nexus_queue_default();
        check(!s.nexus_queue_downloads(),
              "apply_nexus_queue_default never overrides a manual choice");
        s.remove_nexus_queue_downloads();
    }
    // apply_nexus_queue_default seeds the tier default only while unset.
    {
        engine::NexusUserInfo sup;
        sup.account_type = engine::NexusUserInfo::AccountType::Supporter;
        auth.set_user_info(sup);
        ui::apply_nexus_queue_default();
        check(s.nexus_queue_downloads() && s.nexus_queue_downloads_set(),
              "apply_nexus_queue_default seeds Supporter -> ON while unset");
        s.remove_nexus_queue_downloads();
        engine::NexusUserInfo prem;
        prem.account_type = engine::NexusUserInfo::AccountType::Premium;
        auth.set_user_info(prem);
        ui::apply_nexus_queue_default();
        check(!s.nexus_queue_downloads() && s.nexus_queue_downloads_set(),
              "apply_nexus_queue_default seeds Premium -> OFF while unset");
        s.remove_nexus_queue_downloads();
    }
    auth.clear_user_info();

    // --- Stored key flips Connect/Disconnect (no network touched). ---
    check(!auth.has_api_key(), "no api key after clear");
    auth.set_api_key("test-key-123");
    check(auth.has_api_key(), "api key persisted (file fallback)");
    {
        auto dlg = make_dialog();
        auto* nexus = nexus_page_of(dlg);
        auto* connect_btn =
            nexus ? nexus->findChild<QPushButton*>("nexusConnect") : nullptr;
        auto* disconnect_btn =
            nexus ? nexus->findChild<QPushButton*>("nexusDisconnect") : nullptr;
        auto* log_list = nexus ? nexus->findChild<QListWidget*>("nexusLog") : nullptr;
        check(connect_btn && !connect_btn->isEnabled() && disconnect_btn &&
                  disconnect_btn->isEnabled(),
              "Connect stays disabled (validation crashes), Disconnect enabled once a key is stored");
        check(log_list && log_list->count() == 1 &&
                  log_list->item(0)->text() == "Connected.",
              "log greets a stored key with 'Connected.'");
    }
    auth.clear_api_key();
    check(!auth.has_api_key(), "api key cleared");

    // --- Manual key dialog (MO2's NexusManualKeyDialog). ---
    {
        ui::NexusManualKeyDialog md;
        auto* edit = md.findChild<QPlainTextEdit*>();
        check(edit != nullptr, "manual-key dialog has a plain-text edit");
        edit->setPlainText("abc-123");
        md.accept();
        check(md.key() == "abc-123", "manual-key dialog returns the entered key");
    }
    {
        ui::NexusManualKeyDialog md;
        md.findChild<QPlainTextEdit*>()->setPlainText("");
        md.accept();
        check(md.key().isEmpty(),
              "empty manual-key entry stays empty (MO2 parity: clears creds)");
    }

    engine::NexusServers::instance().clear_all();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
