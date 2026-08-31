#include "core/core.h"

#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QMessageLogContext>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleFactory>
#include <QTranslator>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "cli/headless_launcher.h"
#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/masterlist_fetch.h"
#include "engine/core/log/crash_handler.h"
#include "engine/core/log/logger.h"
#include "engine/game/detect/game_detector.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/platform/theme/theme_manager.h"
#include "engine/source/loverslab_auth.h"
#include "engine/source/nexus_auth.h"
#include "engine/source/nxm/managed_games.h"
#include "engine/source/nxm/nxm_router.h"
#include "platform/platform.h"
#include "ui/app/multi_process.h"
#include "ui/game_selection/game_selection_widget.h"
#include "ui/main_window/main_window.h"
#include "ui/nxm/nxm_ipc.h"
#include "ui/settings/settings.h"
#include "ui/theme/icon_manager.h"
#include "ui/theme/style_manager.h"
#include "ui/widgets/game_icon_cache.h"

#ifdef GMM_HAS_QTKEYCHAIN
#include "keyring/qtkeychain_keyring.h"
#endif

#if defined(GMM_PLATFORM_LINUX)
#include "platform/linux/linux_platform.h"
#elif defined(GMM_PLATFORM_WINDOWS)
#include "platform/windows/windows_platform.h"
#endif

namespace fs = std::filesystem;

namespace {

std::string data_dir() {
  return engine::safe_home_dir().string() + "/.local/share/GameModManager";
}

std::string log_path() { return data_dir() + "/gamemodmanager.log"; }

void qt_message_filter(QtMsgType type, const QMessageLogContext &ctx,
                       const QString &msg) {
  // Suppress noisy Qt platform/theme messages
  if (msg.contains("grabbing the mouse") ||
      msg.contains("This plugin supports"))
    return;
  // Forward everything else to the default handler
  fprintf(stderr, "%s\n", qPrintable(msg));
  Q_UNUSED(type);
  Q_UNUSED(ctx);
}

} // namespace

namespace Core {

Application::Application(int &argc, char **argv)
    : app_(argc, argv), command_line_(argc, argv) {
  // Install the cross-platform crash handler as early as possible, before any
  // UI or engine setup, so crashes during startup are captured too.
  engine::install_crash_handler();

  app_.setApplicationName("GameModManager");
  app_.setApplicationVersion(VERSION);

  // Central icon resolution (icon packs). Set up before the window icon so
  // the app icon itself resolves through the pack chain.
  {
    auto &icon_mgr = engine::IconManager::instance();
    icon_mgr.discover_packs(
        QCoreApplication::applicationDirPath().toStdString());
    icon_mgr.set_mode(Settings::instance().icon_pack().toStdString());
    icon_mgr.set_current_theme(Settings::instance().theme().toStdString());
  }

  // App window icon: the PNG (the SVG renderer mis-renders on some setups).
  app_.setWindowIcon(engine::IconManager::instance().resolve_icon("gmm-logo"));

  // Store secrets in the OS keyring (QtKeychain: Secret Service / KWallet)
  // when available, falling back to insecure file storage with a warning
  // otherwise. Must run on the main thread - do it now, before any download
  // worker can touch the key.
#ifdef GMM_HAS_QTKEYCHAIN
  engine::Source::Nexus::Auth::instance().set_keyring(
      std::make_unique<engine::QtKeychainKeyring>());
  engine::LoversLabAuth::instance().set_keyring(
      std::make_unique<engine::QtKeychainKeyring>());
#endif

  // Load translations. The language comes from settings ("language", a
  // language-COUNTRY tag like "en_US" or "de_DE"); English is the source
  // language and ships as a no-op translation, so every .qm lives at
  // :/i18n/<language_COUNTRY>.qm (named after the .ts, e.g. en_US.qm).
  QTranslator translator;
  const QString language = Settings::instance().language();
  if (translator.load(":/i18n/" + language + ".qm"))
    app_.installTranslator(&translator);

  // Suppress noisy Qt platform/theme messages (e.g. "grabbing the mouse" on
  // Wayland)
  qInstallMessageHandler(qt_message_filter);

  // Parse command line
  if (!command_line_.parse())
    return; // --help was printed

  // Initialize logger
  engine::Logger::instance().set_log_file(log_path());
  engine::Logger::instance().info("GameModManager v" + std::string(VERSION) +
                                  " started");

  // Platform services (Steam/Proton discovery, prefix resolution, user dirs).
#if defined(GMM_PLATFORM_LINUX)
  platform_ = std::make_unique<engine::LinuxPlatform>();
#elif defined(GMM_PLATFORM_WINDOWS)
  platform_ = std::make_unique<engine::WindowsPlatform>();
#endif

  // Apply app settings that affect startup behavior.
  auto &settings = Settings::instance();
  const QString instances_dir = settings.instances_dir();
  if (!instances_dir.isEmpty())
    engine::set_instances_dir_override(instances_dir.toStdString());
  const QString log_level = settings.log_level();
  if (log_level == "debug")
    engine::Logger::instance().set_level(engine::LogLevel::Debug);
  else if (log_level == "warn")
    engine::Logger::instance().set_level(engine::LogLevel::Warn);
  else if (log_level == "error")
    engine::Logger::instance().set_level(engine::LogLevel::Error);
  // "info" is the default.

  // Initialize theme system - default uses palette() so desktop colors apply.
  // Capture the native (platform) style name in canonical QStyleFactory
  // casing before any user-selected style is applied.
  native_style_name_ = app_.style()->objectName();
  for (const auto &key : QStyleFactory::keys()) {
    if (key.compare(native_style_name_, Qt::CaseInsensitive) == 0) {
      native_style_name_ = key;
      break;
    }
  }
  theme_manager_ = std::make_unique<engine::ThemeManager>();
  theme_manager_->discover_themes(
      QApplication::applicationDirPath().toStdString());
  style_manager_ = std::make_unique<engine::StyleManager>(*theme_manager_);
  const QString qt_style = Settings::instance().style();
  if (!qt_style.isEmpty()) {
    if (QStyle *st = QStyleFactory::create(qt_style)) {
      app_.setStyle(st);
      engine::Logger::instance().info("Applied Qt style: " +
                                      qt_style.toStdString());
    } else {
      engine::Logger::instance().warn("Unknown Qt style: " +
                                      qt_style.toStdString());
      style_manager_->apply_theme(Settings::instance().theme().toStdString());
    }
  } else {
    style_manager_->apply_theme(Settings::instance().theme().toStdString());
  }

  // Compute pending URL from parsed args
  const auto &args = command_line_.args();
  if (args.handle_nxm) {
    pending_url_ = args.nxm_url.toStdString();
  } else if (args.handle_gmm) {
    std::string raw = args.gmm_url.toStdString();
    static const std::string gmm_prefix = "gmm://nexus/";
    if (raw.compare(0, gmm_prefix.size(), gmm_prefix) == 0) {
      pending_url_ = "nxm://" + raw.substr(gmm_prefix.size());
    } else {
      engine::Logger::instance().warn("Unknown gmm:// scheme: " + raw);
    }
  }

  // Ensure data directories exist
  std::error_code ec;
  fs::create_directories(fs::path(data_dir()), ec);
  fs::create_directories(engine::default_instances_dir(), ec);

  // Load plugins - needed for both headless and GUI modes
  plugin_loader_ = std::make_unique<engine::PluginLoader>();
  std::vector<std::string> disabled;
  for (const auto &name : Settings::instance().disabled_plugins())
    disabled.push_back(name.toStdString());
  plugin_loader_->set_disabled_plugins(disabled);

  auto app_dir = QApplication::applicationDirPath();
  QStringList plugin_dirs = {
      app_dir + "/plugins",
      app_dir + "/../plugins",
  };
  for (const auto &dir : plugin_dirs) {
    if (QDir(dir).exists()) {
      plugin_loader_->load_directory(dir.toStdString());
    }
  }

  // Fetch masterlists for all registered games. This runs after plugin
  // loading so the masterlist_url hooks are registered.
  for (const auto &game_id : plugin_loader_->knowledge().registered_games()) {
    std::string url =
        engine::masterlist_url_for(plugin_loader_->knowledge(), game_id);
    if (url.empty())
      continue;

    std::string error;
    if (!engine::ensure_masterlist_cached(game_id, url, error)) {
      engine::Logger::instance().warn("Masterlist fetch failed for " + game_id +
                                      ": " + error);
    } else {
      engine::Logger::instance().debug("Masterlist cached for " + game_id);
    }
  }
}

Application::~Application() { engine::CrashHandler::uninstall(); }

int Application::run() {
  // If --help was printed during construction, exit early
  if (command_line_.should_exit())
    return command_line_.exit_code();

  const auto &args = command_line_.args();

  // -- Headless launch mode ---------------------------------------------
  if (args.headless) {
    engine::Logger::instance().debug("GameModManager - headless launch");

    std::string instance_str = args.instance_name.toStdString();
    fs::path instance_root = engine::resolve_instance_path(instance_str);
    if (instance_root.empty()) {
      engine::Logger::instance().error(
          "Instance not found: " +
          (instance_str.empty() ? "(none)" : instance_str));
      fprintf(stderr,
              "GameModManager: instance not found. Use --instance <path>.\n");
      return 1;
    }

    engine::Instance inst = engine::Instance::installed(
        instance_root.filename().string(), instance_root.parent_path());
    if (!inst.read_toml()) {
      engine::Logger::instance().error("Failed to read instance.toml at " +
                                       instance_root.string());
      return 1;
    }
    if (inst.info().game_dir.empty()) {
      engine::Logger::instance().error(
          "game_dir not set in instance.toml for " + instance_root.string());
      return 1;
    }
    if (!fs::exists(inst.info().game_dir)) {
      engine::Logger::instance().error("game_dir does not exist: " +
                                       inst.info().game_dir.string());
      return 1;
    }

    std::string exe_rel = args.exe_path.toStdString();
    if (exe_rel.empty()) {
      engine::Logger::instance().error(
          "No executable specified. Use --exe <path>.");
      return 1;
    }
    // Resolve against the canonical game dir spelling
    std::error_code ce;
    auto canon_game = fs::weakly_canonical(inst.info().game_dir, ce);
    auto exec_path =
        (ce || canon_game.empty() ? inst.info().game_dir : canon_game) /
        exe_rel;

    bool is_windows_exe = false;
    auto ext = exec_path.extension().string();
    if (!ext.empty() && ext[0] == '.') {
      auto lower = ext.substr(1);
      for (auto &c : lower)
        c = std::tolower(c);
      is_windows_exe = (lower == "exe");
    }

    // Look up steam_appid from plugin game knowledge
    uint32_t steam_appid = inst.info().steam_appid;
    if (steam_appid == 0) {
      auto id_str = plugin_loader_->knowledge().get(inst.info().game_id,
                                                    "steam_appid", "");
      if (!id_str.empty()) {
        try {
          steam_appid = std::stoul(id_str);
        } catch (...) {
        }
      }
    }

    cli::HeadlessLauncher::Config cfg;
    cfg.executable = exec_path;
    cfg.game_dir = inst.info().game_dir;
    cfg.instance_root = inst.info().root;
    cfg.steam_appid = steam_appid;
    cfg.is_windows_exe = is_windows_exe;
    cfg.knowledge = &plugin_loader_->knowledge();
    cfg.game_id = inst.info().game_id;

    cli::HeadlessLauncher launcher(cfg, platform_.get());
    int exit_code = launcher.run();
    engine::Logger::instance().debug("Headless: exit code " +
                                     std::to_string(exit_code));
    return exit_code;
  }

  // Load the managed games registry (which games we handle nxm:// for)
  engine::ManagedGames managed_games(fs::path(data_dir()) /
                                     "managed_games.json");
  managed_games.load();

  // Check for existing instances
  auto existing_instances = engine::scan_instances();

  // Mutable instance name - may be set by NXM/GMM URL handling below
  QString instance_name = args.instance_name;

  // -- Download URL handling: try IPC to running instance first --
  if (args.handle_nxm || args.handle_gmm) {
    auto link = engine::NxmRouter::parse(pending_url_);
    if (!link.valid()) {
      engine::Logger::instance().error("Invalid download URL: " + pending_url_);
      return 1;
    }

    // Try to send to a running GMM instance via local socket
    if (engine::send_nxm_to_running_instance(
            QString::fromStdString(pending_url_))) {
      engine::Logger::instance().info(
          "Download URL forwarded to running instance");
      return 0;
    }

    // No running instance - resolve the game and find/create the instance
    std::string matched_game_id;
    for (const auto &p : plugin_loader_->plugins()) {
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
    for (const auto &inst_name : existing_instances) {
      auto inst = engine::Instance::installed(inst_name,
                                              engine::default_instances_dir());
      if (inst.read_toml() && inst.info().game_id == matched_game_id) {
        target_instance = inst_name;
        break;
      }
    }

    if (target_instance.empty()) {
      auto display_name = plugin_loader_->display_name_for(matched_game_id);
      engine::Logger::instance().error("No instance exists for " +
                                       display_name + " (" + matched_game_id +
                                       ")");
      fprintf(stderr,
              "GameModManager: no instance exists for '%s'. "
              "Open GameModManager and create an instance first.\n",
              display_name.c_str());
      return 1;
    }

    // We have the instance - fall through to normal GUI startup with this
    // instance
    instance_name = QString::fromStdString(target_instance);
    engine::Logger::instance().debug("Download URL: resolved to instance " +
                                     target_instance +
                                     " (game=" + matched_game_id + ")");
  }

  // -- Single-instance guard (GUI mode only, not headless) ----------------
  engine::MultiProcess instance_guard;
  if (!instance_guard.tryAcquire()) {
    instance_guard.requestFocus();
    engine::Logger::instance().info(
        "Another instance running - requesting focus");
    return 0;
  }

  // Game icons (instance switcher / game selector) resolve their declared
  // URLs from game knowledge - point the shared icon cache at the loaded
  // plugins' store.
  ui::GameIconCache::set_knowledge(&plugin_loader_->knowledge());

  // Determine if we need the game selection screen
  bool show_selection = existing_instances.empty() && instance_name.isEmpty();

  if (show_selection) {
    // -- First-run: show game selection screen --
    engine::Logger::instance().info(
        "No instances found - showing game selection");

    // Detect installed games via Steam VDF
    std::vector<engine::DetectedGame> installed_games;
    {
      std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>>
          game_specs;
      for (const auto &plugin : plugin_loader_->game_plugins()) {
        if (plugin.steam_appid > 0) {
          game_specs.push_back(
              {plugin.steam_appid, {plugin.game_id, plugin.game_display_name}});
        }
      }
      if (!game_specs.empty()) {
        installed_games =
            engine::GameDetector::detect_steam_games_multi(game_specs);
      }
    }

    // Build installed games list
    std::vector<ui::GameEntry> installed_entries;
    for (const auto &g : installed_games) {
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
    for (const auto &plugin : plugin_loader_->game_plugins()) {
      bool found = false;
      for (const auto &g : installed_games) {
        if (g.game_id == plugin.game_id) {
          found = true;
          break;
        }
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
    auto *selection = new ui::GameSelectionWidget();
    selection->set_games(installed_entries, available_entries);

    ui::MainWindow *main_window = nullptr;

    QObject::connect(
        selection, &ui::GameSelectionWidget::game_selected,
        [&](const ui::GameEntry &entry) {
          engine::Logger::instance().info("Game selected: " + entry.game_id);

          const std::string inst_name = ui::prompt_instance_name(
              &stack, QString::fromStdString(entry.display_name));
          if (inst_name.empty())
            return;

          // Find the full DetectedGame for this entry
          engine::DetectedGame detected;
          bool is_detected = false;
          for (const auto &g : installed_games) {
            if (g.game_id == entry.game_id) {
              detected = g;
              is_detected = true;
              break;
            }
          }

          if (!is_detected) {
            detected.game_id = entry.game_id;
            detected.name = entry.display_name;
            detected.steam_appid = entry.steam_appid;
          }

          // Create the instance
          auto new_inst = engine::create_instance_for_game(
              detected, engine::default_instances_dir(), inst_name);
          if (new_inst.info().game_id.empty())
            return;
          engine::write_last_instance(new_inst.info().root.filename().string());
          Settings::instance().ensure_modlist_column_defaults(
              QString::fromStdString(new_inst.info().root.filename().string()));

          // Transition to MainWindow
          main_window = new ui::MainWindow();
          main_window->set_game_knowledge(&plugin_loader_->knowledge());
          main_window->set_plugin_loader(plugin_loader_.get());
          main_window->set_managed_games(&managed_games);
          main_window->set_style_manager(style_manager_.get());
          main_window->set_platform(platform_.get());
          main_window->set_native_style_name(native_style_name_);

          // Forward focus requests from other instances to this window
          QObject::connect(&instance_guard,
                           &engine::MultiProcess::focusRequested, main_window,
                           [main_window]() {
                             main_window->raise();
                             main_window->activateWindow();
                           });

          main_window->set_game_info(detected.game_id, detected.name, "Default",
                                     detected.install_path,
                                     new_inst.info().root);
          main_window->show();
          main_window->apply_initial_geometry();
          stack.hide();

          // If a download link was passed, queue it for this instance
          if (!pending_url_.empty()) {
            main_window->handle_nxm_download(
                engine::NxmRouter::parse(pending_url_));
            pending_url_.clear();
          }
        });

    stack.addWidget(selection);
    stack.setCurrentWidget(selection);
    stack.resize(900, 600);
    stack.setWindowTitle(
        QCoreApplication::translate("main", "GameModManager - Select a Game"));
    stack.show();

    int rc = app_.exec();

    engine::Logger::instance().info("GameModManager shutting down");
    delete main_window;
    return rc;
  }

  // -- Normal startup: instance exists --
  ui::MainWindow window;
  window.set_game_knowledge(&plugin_loader_->knowledge());
  window.set_plugin_loader(plugin_loader_.get());
  window.set_managed_games(&managed_games);
  window.set_style_manager(style_manager_.get());
  window.set_platform(platform_.get());
  window.set_native_style_name(native_style_name_);

  // Forward focus requests from other instances to this window
  QObject::connect(&instance_guard, &engine::MultiProcess::focusRequested,
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
        !fs::exists(engine::default_instances_dir() / active_instance /
                    "instance.toml")) {
      if (!existing_instances.empty()) {
        active_instance = existing_instances[0];
      }
    }
  }

  if (!active_instance.empty()) {
    engine::write_last_instance(active_instance);

    auto temp_inst = engine::Instance::installed(
        active_instance, engine::default_instances_dir());
    temp_inst.read_toml();
    std::string game_id = temp_inst.info().game_id;
    std::filesystem::path game_dir = temp_inst.info().game_dir;

    // Resolve canonical game_id (handles legacy shortname renames)
    std::string resolved_id = plugin_loader_->resolve_game_id(game_id);
    if (resolved_id != game_id) {
      engine::Logger::instance().debug("Migrated game_id: " + game_id + " -> " +
                                       resolved_id);
      temp_inst.info().game_id = resolved_id;
      temp_inst.write_toml();
      game_id = resolved_id;
    }

    std::string display_name = plugin_loader_->display_name_for(game_id);

    window.set_game_info(game_id, display_name, "Default", game_dir,
                         engine::default_instances_dir() / active_instance);
    window.setWindowTitle(
        QCoreApplication::translate("main", "GameModManager - %1")
            .arg(QString::fromStdString(display_name)));
  }

  window.show();
  window.apply_initial_geometry();

  // If a download link was passed, queue it for the active instance
  if (!pending_url_.empty()) {
    window.handle_nxm_download(engine::NxmRouter::parse(pending_url_));
  }

  int rc = app_.exec();

  engine::Logger::instance().info("GameModManager shutting down");
  return rc;
}

} // namespace Core
