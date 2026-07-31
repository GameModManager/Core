#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageLogContext>
#include <QSettings>
#include <QStackedWidget>
#include <QTranslator>

#include <cstdio>
#include <cstdlib>

#include "engine/single_instance.h"

static void qt_message_filter(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    // Suppress noisy Qt platform/theme messages
    if (msg.contains("grabbing the mouse") || msg.contains("This plugin supports"))
        return;
    // Forward everything else to the default handler
    fprintf(stderr, "%s\n", qPrintable(msg));
    Q_UNUSED(type); Q_UNUSED(ctx);
}

#include "ui/main_window/main_window.h"
#include "ui/game_selection/game_selection_widget.h"
#include "engine/log/logger.h"
#include "engine/log/crash_handler.h"
#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/detect/game_detector.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/nxm/nxm_router.h"
#include "engine/nxm/managed_games.h"
#include "engine/nxm/nxm_ipc.h"
#include "engine/theme/theme_manager.h"
#include "engine/theme/style_manager.h"
#include "cli/headless_launcher.h"

#include <cstdlib>
#include <filesystem>

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

int main(int argc, char *argv[])
{
    engine::CrashHandler::install(crash_dir());

    QApplication app(argc, argv);
    app.setApplicationName("GameModManager");
    app.setApplicationVersion("0.1.0");
    // Smooth scrolling on item views is applied per-window via
    // ui::enable_smooth_scrolling() (Qt 6 removed AA_SmoothScrolling).
    // TODO: gate behind a Settings "Smooth scrolling" checkbox.
    // Use platform-native style (Breeze on KDE, etc.)

    // Load translations. The language comes from settings ("language", a
    // language-COUNTRY tag like "en_US" or "de_DE"); English is the source
    // language and ships as a no-op translation, so every .qm lives at
    // :/i18n/<language_COUNTRY>.qm (named after the .ts, e.g. en_US.qm).
    QTranslator translator;
    const QString language = QSettings("GameModManager", "GameModManager")
                                 .value("language", "en_US").toString();
    if (translator.load(":/i18n/" + language + ".qm"))
        app.installTranslator(&translator);

    // Initialize theme system — default uses palette() so desktop colors apply
    engine::ThemeManager theme_manager;
    engine::StyleManager style_manager(theme_manager);
    style_manager.apply_default();

    // Suppress noisy Qt platform/theme messages (e.g. "grabbing the mouse" on Wayland)
    qInstallMessageHandler(qt_message_filter);

    // Parse CLI flags
    QCommandLineParser parser;
    parser.setApplicationDescription("GameModManager - Cross-platform game mod manager");
    parser.addVersionOption();

    QCommandLineOption helpOpt("help", "Show this help message");
    QCommandLineOption helpShort("h", "Show this help message");
    parser.addOption(helpOpt);
    parser.addOption(helpShort);

    QCommandLineOption instanceOpt("instance", "Load specific instance by name", "name");
    parser.addOption(instanceOpt);

    QCommandLineOption launchOpt("launch", "Launch game directly (headless mode)");
    parser.addOption(launchOpt);

    QCommandLineOption exeOpt("exe", "Executable path relative to game dir", "path");
    parser.addOption(exeOpt);

    QCommandLineOption nxmOpt("handle-nxm", "Handle an nxm:// download link", "url");
    parser.addOption(nxmOpt);

    QCommandLineOption gmmOpt("handle-gmm", "Handle a gmm:// download link", "url");
    parser.addOption(gmmOpt);

    parser.process(app);

    // If --help was requested, print colored help and exit
    if (parser.isSet(helpOpt) || parser.isSet(helpShort)) {
        // ANSI color codes
        constexpr const char* R = "\033[31m";   // red - headers
        constexpr const char* G = "\033[32m";   // green - short flags
        constexpr const char* O = "\033[38;5;208m"; // orange - long flags
        constexpr const char* B = "\033[34m";   // blue - variables
        constexpr const char* D = "\033[0m";    // default reset

        // Header
        fprintf(stdout, "%sUsage:%s\n", R, D);
        fprintf(stdout, "  gamemodmanager [options]\n\n");

        // Examples
        fprintf(stdout, "%sExamples:%s\n", R, D);
        fprintf(stdout, "  gamemodmanager                         # Start GUI with last-used instance\n");
        fprintf(stdout, "  gamemodmanager %s--instance%s %s<path>%s   Start GUI with a specific instance\n", O, D, B, D);
        fprintf(stdout, "  gamemodmanager %s--handle-nxm%s %s<url>%s      # Handle an nxm:// download link\n", O, D, B, D);
        fprintf(stdout, "  gamemodmanager %s--handle-gmm%s %s<url>%s      # Handle a gmm:// download link\n", O, D, B, D);
        fprintf(stdout, "  gamemodmanager %s--launch%s %s--instance%s %s<path>%s %s--exe%s %s<path>%s\n", O, D, O, D, B, D, O, D, B, D);
        fprintf(stdout, "                                # Launch game headless\n\n");

        // Options
        fprintf(stdout, "%sOptions:%s\n", R, D);
        fprintf(stdout, "  %s-h%s, %s--help%s          Show this help message\n", G, D, O, D);
        fprintf(stdout, "  %s-v%s, %s--version%s       Show version information\n", G, D, O, D);
        fprintf(stdout, "  %s--instance%s %s<path>%s   Instance path or name\n", O, D, B, D);
        fprintf(stdout, "  %s--launch%s            Launch game directly (headless)\n", O, D);
        fprintf(stdout, "  %s--exe%s %s<path>%s      Executable path relative to game dir\n", O, D, B, D);
        fprintf(stdout, "  %s--handle-nxm%s %s<url>%s  Handle an nxm:// download link\n", O, D, B, D);
        fprintf(stdout, "  %s--handle-gmm%s %s<url>%s  Handle a gmm:// download link\n", O, D, B, D);
        return 0;
    }

    // Initialize logger
    engine::Logger::instance().set_log_file(log_path());
    engine::Logger::instance().info("GameModManager v" + std::string(VERSION) + " started");

    // Parse remaining flags
    bool headless = parser.isSet(launchOpt);
    bool handle_nxm = parser.isSet(nxmOpt);
    bool handle_gmm = parser.isSet(gmmOpt);
    QString instance_name;
    if (parser.isSet(instanceOpt)) {
        instance_name = parser.value(instanceOpt);
    }

    std::string pending_url;
    if (handle_nxm) {
        pending_url = parser.value(nxmOpt).toStdString();
    } else if (handle_gmm) {
        std::string raw = parser.value(gmmOpt).toStdString();
        static const std::string gmm_prefix = "gmm://nexus/";
        if (raw.compare(0, gmm_prefix.size(), gmm_prefix) == 0) {
            pending_url = "nxm://" + raw.substr(gmm_prefix.size());
        } else {
            engine::Logger::instance().warn("Unknown gmm:// scheme: " + raw);
        }
    }

    // Ensure data directories exist
    std::error_code ec;
    fs::create_directories(fs::path(data_dir()), ec);
    fs::create_directories(engine::default_instances_dir(), ec);

    // Load plugins - needed for both headless and GUI modes
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

    // -- Headless launch mode ---------------------------------------------
    if (headless) {
        engine::Logger::instance().debug("GameModManager - headless launch");

        std::string instance_str = instance_name.toStdString();
        fs::path instance_root = engine::resolve_instance_path(instance_str);
        if (instance_root.empty()) {
            engine::Logger::instance().error(
                "Instance not found: " + (instance_str.empty() ? "(none)" : instance_str));
            fprintf(stderr, "GameModManager: instance not found. Use --instance <path>.\n");
            return 1;
        }

        engine::Instance inst = engine::Instance::installed(
            instance_root.filename().string(), instance_root.parent_path());
        if (!inst.read_toml()) {
            engine::Logger::instance().error(
                "Failed to read instance.toml at " + instance_root.string());
            return 1;
        }
        if (inst.info().game_dir.empty()) {
            engine::Logger::instance().error(
                "game_dir not set in instance.toml for " + instance_root.string());
            return 1;
        }
        if (!fs::exists(inst.info().game_dir)) {
            engine::Logger::instance().error(
                "game_dir does not exist: " + inst.info().game_dir.string());
            return 1;
        }

        std::string exe_rel = parser.value(exeOpt).toStdString();
        if (exe_rel.empty()) {
            engine::Logger::instance().error("No executable specified. Use --exe <path>.");
            return 1;
        }
        auto exec_path = inst.info().game_dir / exe_rel;
        if (!fs::exists(exec_path)) {
            engine::Logger::instance().error(
                "Executable not found: " + exec_path.string());
            return 1;
        }

        bool is_windows_exe = false;
        auto ext = exec_path.extension().string();
        if (!ext.empty() && ext[0] == '.') {
            auto lower = ext.substr(1);
            for (auto& c : lower) c = std::tolower(c);
            is_windows_exe = (lower == "exe");
        }

        // Look up steam_appid from plugin game knowledge (same as UI path)
        uint32_t steam_appid = inst.info().steam_appid;
        if (steam_appid == 0) {
            auto id_str = plugin_loader.knowledge().get(
                inst.info().game_id, "steam_appid", "");
            if (!id_str.empty()) {
                try { steam_appid = std::stoul(id_str); } catch (...) {}
            }
        }

        cli::HeadlessConfig cfg;
        cfg.executable = exec_path;
        cfg.game_dir = inst.info().game_dir;
        cfg.instance_root = inst.info().root;
        cfg.steam_appid = steam_appid;
        cfg.is_windows_exe = is_windows_exe;
        cfg.knowledge = &plugin_loader.knowledge();
        cfg.game_id = inst.info().game_id;

        int exit_code = cli::launch_game_headless(cfg);
        engine::Logger::instance().debug(
            "Headless: exit code " + std::to_string(exit_code));
        engine::CrashHandler::uninstall();
        return exit_code;
    }

    // Load the managed games registry (which games we handle nxm:// for)
    engine::ManagedGames managed_games(fs::path(data_dir()) / "managed_games.json");
    managed_games.load();

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
    auto existing_instances = engine::scan_instances();

    // -- Download URL handling: try IPC to running instance first --
    if (handle_nxm || handle_gmm) {
        auto link = engine::NxmRouter::parse(pending_url);
        if (!link.valid()) {
            engine::Logger::instance().error("Invalid download URL: " + pending_url);
            return 1;
        }

        // Try to send to a running GMM instance via local socket
        if (engine::send_nxm_to_running_instance(QString::fromStdString(pending_url))) {
            engine::Logger::instance().info("Download URL forwarded to running instance");
            engine::CrashHandler::uninstall();
            return 0;
        }

        // No running instance - resolve the game and find/create the instance ourselves
        // Match the nexus_domain to a game_id via plugins
        std::string matched_game_id;
        for (const auto& p : plugin_loader.plugins()) {
            if (p.nexus_domain == link.nexus_domain) {
                matched_game_id = p.game_id;
                break;
            }
        }

        if (matched_game_id.empty()) {
            engine::Logger::instance().error(
                "No game plugin supports Nexus domain: " + link.nexus_domain);
            fprintf(stderr, "GameModManager: no game supports Nexus domain '%s'\n",
                    link.nexus_domain.c_str());
            return 1;
        }

        // Find an existing instance for this game
        std::string target_instance;
        for (const auto& inst_name : existing_instances) {
            auto inst = engine::Instance::installed(inst_name, engine::default_instances_dir());
            if (inst.read_toml() && inst.info().game_id == matched_game_id) {
                target_instance = inst_name;
                break;
            }
        }

        if (target_instance.empty()) {
            auto display_name = plugin_loader.display_name_for(matched_game_id);
            engine::Logger::instance().error(
                "No instance exists for " + display_name + " (" + matched_game_id + ")");
            fprintf(stderr, "GameModManager: no instance exists for '%s'. "
                    "Open GameModManager and create an instance first.\n",
                    display_name.c_str());
            return 1;
        }

        // We have the instance - fall through to normal GUI startup with this instance
        instance_name = QString::fromStdString(target_instance);
        engine::Logger::instance().debug(
            "Download URL: resolved to instance " + target_instance + " (game=" + matched_game_id + ")");
    }

    // -- Single-instance guard (GUI mode only, not headless) ----------------
    engine::SingleInstanceGuard instance_guard;
    if (!headless && !instance_guard.tryAcquire()) {
        instance_guard.requestFocus();
        engine::Logger::instance().info("Another instance running - requesting focus");
        engine::CrashHandler::uninstall();
        return 0;
    }

    // Determine if we need the game selection screen
    bool show_selection = existing_instances.empty() && instance_name.isEmpty();

    if (show_selection) {
        // -- First-run: show game selection screen --
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
                auto new_inst = engine::create_instance_for_game(detected);
                if (new_inst.info().game_id.empty()) return;
                engine::write_last_instance(new_inst.info().root.filename().string());

                // Transition to MainWindow
                main_window = new ui::MainWindow();
                main_window->set_game_knowledge(&plugin_loader.knowledge());
                main_window->set_plugin_loader(&plugin_loader);
                main_window->set_managed_games(&managed_games);
                main_window->set_style_manager(&style_manager);

                // Forward focus requests from other instances to this window
                QObject::connect(&instance_guard, &engine::SingleInstanceGuard::focusRequested,
                    main_window, [main_window]() {
                        main_window->raise();
                        main_window->activateWindow();
                    });

                main_window->set_game_info(
                    detected.game_id, detected.name, "Default",
                    detected.install_path, new_inst.info().root);
                main_window->show();
                main_window->apply_initial_geometry();
                stack.hide();

                // If a download link was passed, queue it for this instance
                if (!pending_url.empty()) {
                    main_window->handle_nxm_download(engine::NxmRouter::parse(pending_url));
                    pending_url.clear();  // only handle once
                }
            });

        stack.addWidget(selection);
        stack.setCurrentWidget(selection);
        stack.resize(900, 600);
        stack.setWindowTitle(QCoreApplication::translate("main", "GameModManager - Select a Game"));
        stack.show();

        int rc = app.exec();

        engine::Logger::instance().info("GameModManager shutting down");
        engine::CrashHandler::uninstall();
        delete main_window;
        return rc;
    }

    // -- Normal startup: instance exists --
    ui::MainWindow window;
    window.set_game_knowledge(&plugin_loader.knowledge());
    window.set_plugin_loader(&plugin_loader);
    window.set_managed_games(&managed_games);
    window.set_style_manager(&style_manager);

    // Forward focus requests from other instances to this window
    QObject::connect(&instance_guard, &engine::SingleInstanceGuard::focusRequested,
        &window, [&window]() {
            window.raise();
            window.activateWindow();
        });

    // Resolve which instance to load
    std::string active_instance;
    if (!instance_name.isEmpty()) {
        active_instance = instance_name.toStdString();
    } else {
        active_instance = engine::read_last_instance();
        if (active_instance.empty() ||
            !fs::exists(engine::default_instances_dir() / active_instance / "instance.toml")) {
            if (!existing_instances.empty()) {
                active_instance = existing_instances[0];
            }
        }
    }

    if (!active_instance.empty()) {
        engine::write_last_instance(active_instance);

        auto temp_inst = engine::Instance::installed(active_instance, engine::default_instances_dir());
        temp_inst.read_toml();
        std::string game_id = temp_inst.info().game_id;
        std::filesystem::path game_dir = temp_inst.info().game_dir;

        // Resolve canonical game_id (handles legacy shortname renames)
        std::string resolved_id = plugin_loader.resolve_game_id(game_id);
        if (resolved_id != game_id) {
            engine::Logger::instance().debug("Migrated game_id: " + game_id + " -> " + resolved_id);
            temp_inst.info().game_id = resolved_id;
            temp_inst.write_toml();
            game_id = resolved_id;
        }

        std::string display_name = plugin_loader.display_name_for(game_id);

        window.set_game_info(game_id, display_name, "Default", game_dir,
                             engine::default_instances_dir() / active_instance);
        window.setWindowTitle(
            QCoreApplication::translate("main", "GameModManager - %1")
                .arg(QString::fromStdString(display_name)));
    }

    window.show();
    window.apply_initial_geometry();

    // If a download link was passed, queue it for the active instance
    if (!pending_url.empty()) {
        window.handle_nxm_download(engine::NxmRouter::parse(pending_url));
    }

    int rc = app.exec();

    engine::Logger::instance().info("GameModManager shutting down");
    engine::CrashHandler::uninstall();
    return rc;
}
