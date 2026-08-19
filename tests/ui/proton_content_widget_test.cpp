// Offscreen GUI smoke test for the mode-agnostic Proton content widget.
//
// Verifies the widget constructs, the runner combo lists every discovered
// Proton runner plus "Automatic", selected_runner() maps the combo back
// correctly (empty = Automatic, name otherwise), the Save/Close buttons emit
// save_requested()/cancel_requested() (the host decides what saving means),
// and the inline deploy progress bar exists (the tab-mode replacement for the
// modal QProgressDialog) and starts hidden.
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "platform/platform_interface.h"
#include "ui/proton/proton_content_widget.h"

#include <QApplication>
#include <QComboBox>
#include <QProgressBar>
#include <QPushButton>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}

// QDialogButtonBox standard buttons carry a mnemonic ampersand ("&Close");
// strip it so text comparisons match the plain label.
QString strip_mnemonic(const QString& text) {
    QString t = text;
    return t.remove(QLatin1Char('&'));
}
}

class StubPlatform : public engine::PlatformInterface {
public:
    std::string platform_name() const override { return "test"; }
    fs::path data_dir() const override { return "/tmp/gmm_proton_content_data"; }
    fs::path config_dir() const override { return "/tmp/gmm_proton_content_config"; }
    fs::path cache_dir() const override { return "/tmp/gmm_proton_content_cache"; }
    fs::path find_steam_root() const override { return {}; }
    bool launch_executable(const fs::path&,
                           const std::vector<std::string>&) const override {
        return false;
    }
    std::vector<ProtonVersionInfo> enumerate_proton_versions() const override {
        return {
            {"Proton 10.0", "/steam/Proton 10.0/proton"},
            {"Proton - Experimental", "/steam/Proton - Experimental/proton"},
        };
    }
    fs::path find_proton_named(const std::string& name) const override {
        if (name == "Proton 10.0") return "/steam/Proton 10.0/proton";
        if (name == "Proton - Experimental") return "/steam/Proton - Experimental/proton";
        return {};
    }
    fs::path find_proton() const override { return "/steam/Proton 10.0/proton"; }
};

TEST_CASE("proton content widget", "[ui]") {
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);

    const std::string plugins_dir = GMM_PLUGINS_DIR;

    StubPlatform platform;
    engine::PluginLoader loader;
    if (!plugins_dir.empty())
        loader.load_directory(plugins_dir);

    ui::ProtonContentWidget widget(&platform, &loader, "SkyrimSpecialEdition",
                                   "Skyrim Special Edition",
                                   "/home/petrica/.steam/steam/steamapps/common/"
                                   "Skyrim Special Edition",
                                   489830, "/tmp/gmm_proton_content_instance", "",
                                   engine::kDefaultDeployStrategy,
                                   engine::DeployConfig{});
    require(widget.selected_runner().empty(), "Automatic maps to empty runner");

    // The runner combo must list Automatic plus the two stubbed runners.
    bool found_auto = false, found_10 = false, found_exp = false;
    for (auto* combo : widget.findChildren<QComboBox*>()) {
        for (int i = 0; i < combo->count(); ++i) {
            const auto text = combo->itemText(i).toStdString();
            if (text == "Automatic (Steam default)") found_auto = true;
            if (text == "Proton 10.0") found_10 = true;
            if (text == "Proton - Experimental") found_exp = true;
        }
    }
    require(found_auto, "combo lists Automatic");
    require(found_10, "combo lists Proton 10.0");
    require(found_exp, "combo lists Proton - Experimental");

    // Inline deploy progress bar exists (tab-mode replacement for the modal
    // QProgressDialog) and starts hidden.
    auto* progress = widget.findChild<QProgressBar*>();
    require(progress != nullptr, "inline progress bar exists");
    require(!progress->isVisible(), "progress bar hidden while idle");

    // Save/Close buttons emit the host-facing signals.
    bool saved = false, cancelled = false;
    QObject::connect(&widget, &ui::ProtonContentWidget::save_requested, &widget,
                     [&saved]() { saved = true; });
    QObject::connect(&widget, &ui::ProtonContentWidget::cancel_requested, &widget,
                     [&cancelled]() { cancelled = true; });
    for (auto* btn : widget.findChildren<QPushButton*>()) {
        if (strip_mnemonic(btn->text()) == "Save") btn->click();
    }
    require(saved, "Save emits save_requested");
    for (auto* btn : widget.findChildren<QPushButton*>()) {
        if (strip_mnemonic(btn->text()) == "Close") btn->click();
    }
    require(cancelled, "Close emits cancel_requested");
}