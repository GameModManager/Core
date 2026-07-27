#include "engine/plugin_host/plugin_loader.h"
#include "engine/plugin_host/python_loader.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

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

int main() {
    std::cout << "Python plugin tests" << std::endl;

    test_python_plugin_load();
    test_python_plugin_missing_register();
    test_python_plugin_duplicate_load();

    std::cout << "\nAll Python plugin tests passed!" << std::endl;
    return 0;
}
