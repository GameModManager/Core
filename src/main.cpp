#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QStackedWidget>

#include "ui/main_window/main_window.h"
#include "ui/game_selection/game_selection_widget.h"
#include "engine/log/logger.h"
#include "engine/log/crash_handler.h"
#include "engine/instance/instance.h"
#include "engine/detect/game_detector.h"
#include "engine/plugin_host/plugin_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string data_dir() {
    const char* home = std::getenv("HOME");
    if (!home) return "/tmp/gamemodmanager";
    return std::string(home) + "/.local/share/GameModManager";
}

static std::string log_path() {
    return data_dir() + "/gamemodmanager.log";
}

static std::string crash_dir() {
    return data_dir() + "/crashes";
}

// Where installed instances live: ~/.local/share/GameModManager/instances/
static std::string instances_dir() {
    return data_dir() + "/instances";
}

// File tracking the last-used instance name
static std::string last_instance_path() {
    return data_dir() + "/last_instance";
}

// Convert a display name to an instance folder name.
// "The Binding of Isaac: Rebirth" → "The_Binding_of_Isaac_Rebirth"
// Strips all characters invalid on Windows: \ / : * ? " < > |
static std::string display_name_to_instance(const std::string& display_name) {
    static const std::string invalid = R"(\/:*?"<>|)";
    std::string result;
    result.reserve(display_name.size());
    for (char c : display_name) {
        if (c == ' ') result += '_';
        else if (invalid.find(c) != std::string::npos) continue;
        else result += c;
    }
    return result;
}

// Scan for existing instances. Returns the list of instance names found.
static std::vector<std::string> scan_instances() {
    std::vector<std::string> result;
    std::error_code ec;
    auto dir = fs::path(instances_dir());
    if (!fs::is_directory(dir, ec)) return result;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        auto toml = entry.path() / "instance.toml";
        if (fs::exists(toml)) {
            result.push_back(entry.path().filename().string());
        }
    }
    return result;
}

// Read the last-used instance name from the tracking file.
static std::string read_last_instance() {
    std::ifstream f(last_instance_path());
    std::string name;
    if (f) std::getline(f, name);
    return name;
}

// Write the last-used instance name to the tracking file.
static void write_last_instance(const std::string& name) {
    std::ofstream f(last_instance_path());
    if (f) f << name << "\n";
}

// Parse game_id from instance.toml
static std::string read_game_id_from_toml(const fs::path& toml_path) {
    std::ifstream f(toml_path);
    std::string line;
    while (std::getline(f, line)) {
        // Look for: game_id = "..."
        auto key_pos = line.find("game_id");
        if (key_pos == std::string::npos) continue;
        auto eq = line.find('=', key_pos);
        if (eq == std::string::npos) continue;
        auto q1 = line.find('"', eq + 1);
        if (q1 == std::string::npos) continue;
        auto q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        return line.substr(q1 + 1, q2 - q1 - 1);
    }
    return {};
}

// Create an instance for the selected game.
// instance_name is derived from the display name (e.g. "The_Binding_of_Isaac_Rebirth").
static bool create_instance(const engine::DetectedGame& game,
                             const std::string& instance_name) {
    engine::Instance inst = engine::Instance::installed(
        instance_name, fs::path(instances_dir()));
    inst.info().game_id = game.game_id;
    inst.info().game_dir = game.install_path;

    if (!inst.create_directories()) {
        engine::Logger::instance().error(
            "Failed to create instance directories for " + instance_name);
        return false;
    }
    if (!inst.write_toml()) {
        engine::Logger::instance().error(
            "Failed to write instance.toml for " + instance_name);
        return false;
    }

    engine::Logger::instance().info(
        "Instance created: " + instance_name + " (game=" + game.game_id +
        ") at " + inst.info().root.string());
    return true;
}

int main(int argc, char *argv[])
{
    engine::CrashHandler::install(crash_dir());

    QApplication app(argc, argv);
    app.setApplicationName("GameModManager");
    app.setApplicationVersion("0.1.0");
    app.setStyle("fusion");

    // Parse CLI flags
    QCommandLineParser parser;
    parser.setApplicationDescription("GameModManager - Cross-platform game mod manager");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption instanceOpt("instance", "Load specific instance", "name");
    parser.addOption(instanceOpt);

    QCommandLineOption launchOpt("launch", "Launch game directly (headless mode)");
    parser.addOption(launchOpt);

    parser.process(app);

    // Initialize logger
    engine::Logger::instance().set_log_file(log_path());
    engine::Logger::instance().info("GameModManager v" + std::string(VERSION) + " started");

    // Handle CLI flags
    bool headless = parser.isSet(launchOpt);
    QString instance_name;
    if (parser.isSet(instanceOpt)) {
        instance_name = parser.value(instanceOpt);
        engine::Logger::instance().info("Instance: " + instance_name.toStdString());
    }

    if (headless) {
        engine::Logger::instance().info("Headless launch mode");
        engine::Logger::instance().error("Headless mode not yet implemented");
        return 1;
    }

    // Ensure data directories exist
    std::error_code ec;
    fs::create_directories(fs::path(data_dir()), ec);
    fs::create_directories(fs::path(instances_dir()), ec);

    // Load plugins to discover supported games
    engine::PluginLoader plugin_loader;
    auto app_dir = QApplication::applicationDirPath();
    QStringList plugin_dirs = {
        app_dir + "/plugins",
        app_dir + "/../plugins",
    };
    for (const auto& dir : plugin_dirs) {
        if (QDir(dir).exists()) {
            plugin_loader.load_directory(dir.toStdString());
        }
    }

    // Detect installed games via Steam VDF
    std::vector<engine::DetectedGame> installed_games;
    {
        // Build a list of (appid, {game_id, game_name}) from loaded plugins
        std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> game_specs;
        for (const auto& plugin : plugin_loader.plugins()) {
            if (plugin.steam_appid > 0) {
                game_specs.push_back({plugin.steam_appid,
                    {plugin.game_id, plugin.game_display_name}});
            }
        }
        if (!game_specs.empty()) {
            installed_games = engine::GameDetector::detect_steam_games_multi(game_specs);
        }
    }

    // Check for existing instances
    auto existing_instances = scan_instances();

    // Determine if we need the game selection screen
    bool show_selection = existing_instances.empty() && instance_name.isEmpty();

    if (show_selection) {
        // ── First-run: show game selection screen ──
        engine::Logger::instance().info("No instances found - showing game selection");

        // Build installed games list
        std::vector<ui::GameEntry> installed_entries;
        for (const auto& g : installed_games) {
            ui::GameEntry e;
            e.game_id = g.game_id;
            e.display_name = g.name;
            e.steam_appid = g.steam_appid;
            e.installed = true;
            e.install_path = g.install_path;
            installed_entries.push_back(e);
        }

        // Build available games list (all registered but not detected)
        std::vector<ui::GameEntry> available_entries;
        for (const auto& plugin : plugin_loader.plugins()) {
            bool found = false;
            for (const auto& g : installed_games) {
                if (g.game_id == plugin.game_id) { found = true; break; }
            }
            if (!found) {
                ui::GameEntry e;
                e.game_id = plugin.game_id;
                e.display_name = plugin.game_display_name;
                e.steam_appid = plugin.steam_appid;
                e.installed = false;
                available_entries.push_back(e);
            }
        }

        // Show game selection, then MainWindow on game selected
        QStackedWidget stack;
        auto* selection = new ui::GameSelectionWidget();
        selection->set_games(installed_entries, available_entries);

        ui::MainWindow* main_window = nullptr;

        QObject::connect(selection, &ui::GameSelectionWidget::game_selected,
            [&](const ui::GameEntry& entry) {
                engine::Logger::instance().info("Game selected: " + entry.game_id);

                // Find the full DetectedGame for this entry
                engine::DetectedGame detected;
                bool is_detected = false;
                for (const auto& g : installed_games) {
                    if (g.game_id == entry.game_id) {
                        detected = g;
                        is_detected = true;
                        break;
                    }
                }

                // If not auto-detected, use entry info
                if (!is_detected) {
                    detected.game_id = entry.game_id;
                    detected.name = entry.display_name;
                    detected.steam_appid = entry.steam_appid;
                }

                // Create the instance
                std::string inst_name = display_name_to_instance(detected.name);
                if (!create_instance(detected, inst_name)) return;
                write_last_instance(inst_name);

                // Transition to MainWindow
                main_window = new ui::MainWindow();
                main_window->set_game_knowledge(&plugin_loader.knowledge());
                auto inst_root = fs::path(instances_dir()) / inst_name;
                main_window->set_game_info(
                    detected.game_id, detected.name, "Default",
                    detected.install_path, inst_root);
                main_window->show();
                stack.hide();
            });

        stack.addWidget(selection);
        stack.setCurrentWidget(selection);
        stack.resize(900, 600);
        stack.setWindowTitle("GameModManager - Select a Game");
        stack.show();

        int rc = app.exec();

        engine::Logger::instance().info("GameModManager shutting down");
        engine::CrashHandler::uninstall();
        delete main_window;
        return rc;
    }

    // ── Normal startup: instance exists ──
    ui::MainWindow window;
    window.set_game_knowledge(&plugin_loader.knowledge());

    // Resolve which instance to load
    std::string active_instance;
    if (!instance_name.isEmpty()) {
        active_instance = instance_name.toStdString();
    } else {
        // Auto-load last-used instance
        active_instance = read_last_instance();
        // Fallback to first found if no last-used or it no longer exists
        if (active_instance.empty() ||
            !fs::exists(fs::path(instances_dir()) / active_instance / "instance.toml")) {
            if (!existing_instances.empty()) {
                active_instance = existing_instances[0];
            }
        }
    }

    if (!active_instance.empty()) {
        write_last_instance(active_instance);

        // Read the game_id and game_dir from the instance's toml
        auto toml = fs::path(instances_dir()) / active_instance / "instance.toml";
        engine::Instance temp_inst = engine::Instance::installed(active_instance, fs::path(instances_dir()));
        temp_inst.read_toml();
        std::string game_id = temp_inst.info().game_id;
        std::filesystem::path game_dir = temp_inst.info().game_dir;
        std::string display_name = plugin_loader.display_name_for(game_id);

        window.set_game_info(game_id, display_name, "Default", game_dir,
                             fs::path(instances_dir()) / active_instance);
        window.setWindowTitle(("GameModManager - " + display_name).c_str());
        engine::Logger::instance().info("Loaded instance: " + active_instance +
            " (game=" + game_id + ")");
    }

    window.show();

    int rc = app.exec();

    engine::Logger::instance().info("GameModManager shutting down");
    engine::CrashHandler::uninstall();
    return rc;
}
