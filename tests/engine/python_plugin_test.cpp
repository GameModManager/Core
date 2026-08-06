#include "engine/plugin_host/plugin_loader.h"
#include "engine/plugin_host/python_loader.h"
#include "engine/plugin_host/diagnostics_registry.h"
#include "engine/plugins/plugin_database.h"
#include "engine/detect/mod_scanner.h"
#include "engine/registry/game_features/game_feature_registry.h"
#include "engine/registry/game_knowledge.h"
#include "engine/events/event_bus.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

// Release builds compile out assert() (-DNDEBUG), so the plugin-loading checks
// above use it loosely; the register_game_feature mirror test below uses this
// hard require (exits non-zero on failure) so ctest actually catches it.
static void require(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
        std::exit(1);
    }
}

static void test_python_plugin_load() {
    std::cout << "=== test_python_plugin_load ===" << std::endl;

    // Create a temp directory with a Python plugin
    fs::path tmp = fs::temp_directory_path() / "gmm_python_test";
    fs::create_directories(tmp);

    // Write a test plugin
    fs::path plugin_path = tmp / "testgame.py";
    {
        std::ofstream f(plugin_path);
        f << R"(
import gmm

def diag(plugin_name):
    if plugin_name == "Target.esp":
        return ["python says hi", "and again"]
    return []

def register(ctx):
    ctx.register_identity(
        steam_appid=12345,
        nexus_domain="testgame",
    )
    ctx.register_capability(
        "plugins",
        display_name="Plugins",
        data_path="Data/",
        description="Test game plugin support",
    )
    ctx.register_diagnostics(diag)
)";
    }

    // Init Python
    bool ok = engine::python_init();
    assert(ok && "python_init failed");

    // Load the plugin
    engine::PluginLoader loader;
    ok = engine::python_load_plugin(&loader, plugin_path.string());
    assert(ok && "python_load_plugin failed");

    // Verify registration
    assert(loader.plugins().size() == 1);
    const auto& info = loader.plugins()[0];
    assert(info.game_id == "testgame");
    assert(info.steam_appid == 12345);
    assert(info.registered);
    assert(info.loaded);

    // Verify capabilities registered
    auto game_caps = loader.capabilities().capabilities_for("testgame");
    assert(game_caps.size() == 1);
    assert(game_caps[0].capability == "plugins");
    assert(game_caps[0].display_name == "Plugins");

    // Verify the Python diagnostics provider: collect() invokes the bridged
    // callable once per plugin and lands its messages in GamePlugin::messages.
    {
        engine::PluginDatabase db;
        auto& ps = db.plugins_mutable();
        engine::GamePlugin target;
        target.name = "Target.esp";
        engine::GamePlugin other;
        other.name = "Other.esp";
        ps.push_back(target);
        ps.push_back(other);

        engine::DiagnosticsRegistry::instance().collect("testgame", db);
        assert(ps.size() == 2);
        assert(ps[0].messages.size() == 2);
        assert(ps[0].messages[0] == "python says hi");
        assert(ps[0].messages[1] == "and again");
        assert(ps[1].messages.empty());
        engine::DiagnosticsRegistry::instance().clear();
    }

    std::cout << "  game_id: " << info.game_id << std::endl;
    std::cout << "  steam_appid: " << info.steam_appid << std::endl;
    std::cout << "  nexus_domain: " << info.nexus_domain << std::endl;
    std::cout << "  capabilities: " << game_caps.size() << std::endl;
    std::cout << "  PASSED" << std::endl;

    // Cleanup
    engine::python_shutdown();
    fs::remove_all(tmp);
}

static void test_python_plugin_missing_register() {
    std::cout << "=== test_python_plugin_missing_register ===" << std::endl;

    fs::path tmp = fs::temp_directory_path() / "gmm_python_test2";
    fs::create_directories(tmp);

    // Plugin without register() function
    fs::path plugin_path = tmp / "badplugin.py";
    {
        std::ofstream f(plugin_path);
        f << "# no register() function\nsome_var = 42\n";
    }

    engine::python_init();

    engine::PluginLoader loader;
    bool ok = engine::python_load_plugin(&loader, plugin_path.string());
    assert(!ok && "should have failed for missing register()");

    assert(loader.plugins().empty());

    std::cout << "  correctly rejected plugin without register()" << std::endl;
    std::cout << "  PASSED" << std::endl;

    engine::python_shutdown();
    fs::remove_all(tmp);
}

static void test_python_plugin_duplicate_load() {
    std::cout << "=== test_python_plugin_duplicate_load ===" << std::endl;

    fs::path tmp = fs::temp_directory_path() / "gmm_python_test3";
    fs::create_directories(tmp);

    fs::path plugin_path = tmp / "dupgame.py";
    {
        std::ofstream f(plugin_path);
        f << R"(
import gmm

def register(ctx):
    ctx.register_identity(steam_appid=99999)
)";
    }

    engine::python_init();

    engine::PluginLoader loader;
    bool ok1 = engine::python_load_plugin(&loader, plugin_path.string());
    assert(ok1 && "first load should succeed");

    bool ok2 = engine::python_load_plugin(&loader, plugin_path.string());
    assert(ok2 && "duplicate load should return true (already loaded)");
    assert(loader.plugins().size() == 1);

    std::cout << "  duplicate load handled correctly" << std::endl;
    std::cout << "  PASSED" << std::endl;

    engine::python_shutdown();
    fs::remove_all(tmp);
}

// P1.2 GameFeatureRegistry pybind mirror: a Python plugin calls
// ctx.register_game_feature() to register (or, with a higher priority,
// override) a per-game mod_data_checker. The mirror must feed the same
// engine registry the C ABI feeds — so a mod whose only content is the
// registered folder is no longer "No valid game data" (FLAG_INVALID).
static void test_python_register_game_feature() {
    std::cout << "=== test_python_register_game_feature ===" << std::endl;

    engine::GameFeatureRegistry::instance().clear();

    fs::path tmp = fs::temp_directory_path() / "gmm_python_feature";
    fs::create_directories(tmp);

    fs::path plugin_path = tmp / "checker_override.py";
    {
        std::ofstream f(plugin_path);
        f << R"(
import gmm

def register(ctx):
    ctx.register_identity(steam_appid=12345)
    ctx.register_game_feature(
        game_id="skyrim",
        feature_type="mod_data_checker",
        priority=5,
        folder_names=["customstuff"],
        file_extensions=["custoext"],
    )
)";
    }

    fs::path root = tmp / "mods";
    fs::create_directories(root / "CustomMod" / "customstuff");
    fs::create_directories(root / "ForeignMod" / "otherstuff");

    engine::python_init();
    engine::PluginLoader loader;
    require(engine::python_load_plugin(&loader, plugin_path.string()),
            "python plugin with register_game_feature loads");

    auto combined = engine::GameFeatureRegistry::instance().resolve_mod_data_checker("skyrim");
    require(combined != nullptr, "python-registered checker resolves");
    require(combined && !combined->folder_names().empty() &&
            combined->folder_names()[0] == "customstuff",
            "python folder_names reached the engine registry");

    auto mods = engine::ModScanner::scan_dir(engine::GameKnowledge{}, "skyrim", root);
    bool found_custom = false, found_foreign = false;
    for (const auto& m : mods) {
        if (m.folder_name == "CustomMod") {
            found_custom = true;
            require(!m.invalid_data,
                    "mod with only customstuff/ is valid (override shows)");
        }
        if (m.folder_name == "ForeignMod") {
            found_foreign = true;
            require(m.invalid_data,
                    "mod with only otherstuff/ is no valid game data");
        }
    }
    require(found_custom && found_foreign, "both mod folders scanned");

    std::cout << "  python register_game_feature feeds the engine registry" << std::endl;
    std::cout << "  PASSED" << std::endl;

    engine::python_shutdown();
    fs::remove_all(tmp);
}

// P1.3 exit criterion: "a Python plugin logs an install and a state-change,
// engine tests pin the event stream." The plugin subscribes to the host event
// bus during register(); the test drives the SAME public dispatch() the UI
// calls, and asserts the Python handler received both events with the right
// dict payloads (logged to a file by the plugin itself).
static void test_python_subscribe_event() {
    std::cout << "=== test_python_subscribe_event ===" << std::endl;

    engine::EventBus::instance().clear();

    fs::path tmp = fs::temp_directory_path() / "gmm_python_events";
    fs::create_directories(tmp);
    fs::path log_path = tmp / "events.log";

    fs::path plugin_path = tmp / "listener.py";
    {
        std::ofstream f(plugin_path);
        f << "import gmm\n";
        f << "\n";
        f << "LOG = " << '"' << log_path.string() << '"' << "\n";
        f << "\n";
        f << R"(
def log_event(event_id, payload):
    with open(LOG, "a") as fh:
        fh.write(event_id + "|" + payload.get("mod", "") + "|"
                 + str(payload.get("enabled", "")) + "\n")

def register(ctx):
    ctx.register_identity(steam_appid=4242)
    ctx.subscribe_event("mod_installed", log_event)
    ctx.subscribe_event("mod_state_changed", log_event)
)";
    }

    engine::python_init();
    engine::PluginLoader loader;
    require(engine::python_load_plugin(&loader, plugin_path.string()),
            "python plugin with subscribe_event loads");
    require(loader.plugins().size() == 1,
            "listener plugin registered");
    require(engine::EventBus::instance().subscriber_count("mod_installed") == 1,
            "python subscription landed on the bus (mod_installed)");
    require(engine::EventBus::instance().subscriber_count("mod_state_changed") == 1,
            "python subscription landed on the bus (mod_state_changed)");

    // Drive the same public emit the UI uses (MainWindow install/state points).
    engine::EventBus::instance().dispatch(
        engine::events::kModInstalled,
        engine::json_obj({{"mod", "SkyUI"}, {"name", "SkyUI"}}));
    engine::EventBus::instance().dispatch(
        engine::events::kModStateChanged,
        engine::json_obj({{"mod", "SkyUI"}, {"enabled", "1"}}));

    // The Python handler logged exactly the two events, in order, with the
    // decoded dict payloads.
    require(fs::exists(log_path), "python handler wrote the events log");
    std::ifstream f(log_path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    require(lines.size() == 2, "install + state-change both logged");
    require(lines[0] == "mod_installed|SkyUI|", "install logged with mod name");
    require(lines[1] == "mod_state_changed|SkyUI|1", "state-change logged with enabled=1");

    std::cout << "  python plugin logged install + state-change via the bus" << std::endl;
    std::cout << "  PASSED" << std::endl;

    engine::python_shutdown();
    engine::EventBus::instance().clear();
    fs::remove_all(tmp);
}

// P1.5: the pybind mirror of register_settings_tab. A Python plugin declares a
// typed settings tab; the loader parses it into PluginInfo::settings_tab the
// same way the C ABI path does (choices split into a list, int range kept as
// a string, no options for bool/string).
static void test_python_settings_tab() {
    std::cout << "=== test_python_settings_tab ===" << std::endl;

    fs::path tmp = fs::temp_directory_path() / "gmm_python_settings_tab";
    fs::create_directories(tmp);

    fs::path plugin_path = tmp / "settings_tab.py";
    {
        std::ofstream f(plugin_path);
        f << R"(
import gmm

def register(ctx):
    ctx.register_identity(nexus_domain="settabs", steam_appid=9999)
    ctx.register_category("Settings Page")
    ctx.register_settings_tab("Python Fixture Settings", [
        ("show_previews", "bool", "1", None),
        ("max_threads", "int", "4", "1:8"),
        ("mod_name_prefix", "string", "mod_", None),
        ("install_mode", "choice", "Full", ["Full", "Compact", "Minimal"]),
    ])
    ctx.register_settings([("plain_legacy_key", "legacy_value")])
)";
    }

    engine::python_init();
    engine::PluginLoader loader;
    require(engine::python_load_plugin(&loader, plugin_path.string()),
            "python plugin with register_settings_tab loads");
    require(loader.plugins().size() == 1, "settings-tab plugin registered");
    const auto& info = loader.plugins()[0];

    require(info.settings_tab.title == "Python Fixture Settings",
            "settings_tab.title parsed from the Python declaration");
    require(info.settings_tab.settings.size() == 4,
            "all four typed settings parsed");

    const auto& show = info.settings_tab.settings[0];
    require(show.key == "show_previews" && show.type == "bool" &&
                show.default_value == "1" && show.choices.empty() &&
                show.int_range.empty(),
            "bool entry parsed (no options)");

    const auto& thr = info.settings_tab.settings[1];
    require(thr.key == "max_threads" && thr.type == "int" &&
                thr.default_value == "4" && thr.int_range == "1:8",
            "int entry parsed with min:max range");

    const auto& pre = info.settings_tab.settings[2];
    require(pre.key == "mod_name_prefix" && pre.type == "string" &&
                pre.default_value == "mod_",
            "string entry parsed");

    const auto& mode = info.settings_tab.settings[3];
    require(mode.key == "install_mode" && mode.type == "choice" &&
                mode.default_value == "Full" &&
                mode.choices == std::vector<std::string>{"Full", "Compact", "Minimal"},
            "choice entry parsed with candidate list");

    require(info.settings.size() == 1 &&
                info.settings.begin()->first == "plain_legacy_key",
            "undeclared register_settings key kept alongside the tab");

    std::cout << "  settings_tab parsed: title='" << info.settings_tab.title
              << "' entries=" << info.settings_tab.settings.size() << std::endl;
    std::cout << "  PASSED" << std::endl;

    engine::python_shutdown();
    fs::remove_all(tmp);
}

// Regression (0.2.35): the create-new-instance / first-run game lists treated
// EVERY plugin with a game_id as a game, so Tool/feature plugins that never
// called register_identity (ImageDiff, IsaacModSorter) showed up as creatable
// games. game_support is set ONLY by register_identity; game_plugins() must
// return exactly the identity-registered plugins.
static void test_python_game_plugins() {
    std::cout << "=== test_python_game_plugins ===" << std::endl;

    engine::GameFeatureRegistry::instance().clear();

    fs::path tmp = fs::temp_directory_path() / "gmm_python_game_plugins";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const fs::path game_path = tmp / "game.py";
    {
        std::ofstream f(game_path);
        f << R"(
import gmm

def register(ctx):
    ctx.register_identity(steam_appid=777, nexus_domain="game")
)";
    }
    const fs::path tool_path = tmp / "tool.py";
    {
        std::ofstream f(tool_path);
        f << R"(
import gmm

def register(ctx):
    ctx.register_meta(author="Team", version="1", description="a tool")
)";
    }

    engine::python_init();
    engine::PluginLoader loader;
    require(engine::python_load_plugin(&loader, game_path.string()),
            "game python plugin loads");
    require(engine::python_load_plugin(&loader, tool_path.string()),
            "tool python plugin loads");
    require(loader.plugins().size() == 2,
            "both plugins registered");

    bool saw_game = false, saw_tool = false;
    for (const auto& p : loader.plugins()) {
        if (p.game_id == "game") {
            saw_game = true;
            require(p.game_support, "register_identity sets game_support");
        } else if (p.game_id == "tool") {
            saw_tool = true;
            require(!p.game_support, "no register_identity means NOT game support");
        }
    }
    require(saw_game && saw_tool, "both plugin infos inspected");

    const auto games = loader.game_plugins();
    require(games.size() == 1 && games[0].game_id == "game",
            "game_plugins() returns ONLY the identity-registered plugin");
    require(games[0].steam_appid == 777 && games[0].game_display_name == "game",
            "game_plugins() carries identity data");

    std::cout << "  game_plugins() filters out non-game tool plugins" << std::endl;
    std::cout << "  PASSED" << std::endl;

    engine::python_shutdown();
    fs::remove_all(tmp);
}

int main() {
    std::cout << "Python plugin tests" << std::endl;

    test_python_plugin_load();
    test_python_plugin_missing_register();
    test_python_plugin_duplicate_load();
    test_python_register_game_feature();
    test_python_subscribe_event();
    test_python_settings_tab();
    test_python_game_plugins();

    std::cout << "\nAll Python plugin tests passed!" << std::endl;
    return 0;
}
