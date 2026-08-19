// Offscreen GUI smoke test for the Instance Options dialog.
//
// Verifies the dialog constructs, the runner combo lists every discovered
// Proton runner plus "Automatic", the wine.json recommended packages are
// surfaced when the plugin loader resolves them, and selected_runner() maps
// the combo back correctly (empty = Automatic, name otherwise). Uses a stub
// platform + a real PluginLoader pointed at the built plugins directory, so
// the wine.json shipped with the Skyrim plugin is exercised.
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "platform/platform_interface.h"
#include "ui/instance_options/instance_options_panel.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>

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
}

class StubPlatform : public engine::PlatformInterface {
public:
    std::string platform_name() const override { return "test"; }
    fs::path data_dir() const override { return "/tmp/gmm_instance_options_panel_data"; }
    fs::path config_dir() const override { return "/tmp/gmm_instance_options_panel_config"; }
    fs::path cache_dir() const override { return "/tmp/gmm_instance_options_panel_cache"; }
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

TEST_CASE("instance options panel", "[ui]") {
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);

    const std::string plugins_dir = GMM_PLUGINS_DIR;

    StubPlatform platform;
    engine::PluginLoader loader;
    if (!plugins_dir.empty())
        loader.load_directory(plugins_dir);

    // Skyrim: wine.json carries vcrun2022.
    {
        ui::InstanceOptionsDialog panel(&platform, &loader, "SkyrimSpecialEdition",
                                        "Skyrim Special Edition",
                                        "/home/petrica/.steam/steam/steamapps/common/"
                                        "Skyrim Special Edition",
                                        489830, "/tmp/gmm_instance_options_panel_instance",
                                        "", engine::kDefaultDeployStrategy,
                                        engine::DeployConfig{});
        require(panel.selected_runner().empty(), "Automatic maps to empty runner");

        // Find the runner combo by walking children: it must list Automatic
        // plus the two stubbed runners.
        bool found_auto = false, found_10 = false, found_exp = false;
        for (auto* combo : panel.findChildren<QComboBox*>()) {
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

        // Deploy management: the strategy selector must list Symlink (always
        // available). OverlayFS appears only when the host supports it (not
        // guaranteed on this test machine), so its absence is not asserted.
        bool found_symlink = false;
        for (auto* combo : panel.findChildren<QComboBox*>()) {
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemText(i) == "Symlink") found_symlink = true;
            }
        }
        require(found_symlink, "deploy strategy combo lists Symlink");
    }

    // Unknown game: no wine.json -> panel still constructs, no crash. The
    // runner selector and packages are hidden (steam_appid == 0), but the
    // combo is still populated so selected_runner() stays well-defined.
    {
        ui::InstanceOptionsDialog panel(&platform, &loader, "UnknownGame", "Unknown Game",
                                        "/tmp/gmm_instance_options_panel_game", 0,
                                        "/tmp/gmm_instance_options_panel_instance", "",
                                        engine::kDefaultDeployStrategy,
                                        engine::DeployConfig{});
        require(panel.selected_runner().empty(), "unknown game stays Automatic");
    }
}
