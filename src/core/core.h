#pragma once

#include <memory>
#include <string>

#include <QApplication>
#include <QString>

#include <filesystem>

#include "cli/command_line.h"

namespace engine {
class Platform;
class PluginLoader;
class ThemeManager;
class StyleManager;
class ManagedGames;
class MultiProcess;
}  // namespace engine

namespace Core {

class Application {
public:
  Application(int& argc, char** argv);
  ~Application();

  int run();

private:
  QApplication app_;
  cli::CommandLine command_line_;

  std::unique_ptr<engine::Platform> platform_;
  std::unique_ptr<engine::PluginLoader> plugin_loader_;
  std::unique_ptr<engine::ThemeManager> theme_manager_;
  std::unique_ptr<engine::StyleManager> style_manager_;

  QString native_style_name_;
  std::string pending_url_;
  // Instance root whose per-instance settings were applied at startup
  // (Workspace-jagw). Empty when no instance was resolvable yet (first run);
  // run() reconciles it against the actually loaded instance and restarts
  // when the effective disabled-plugins differ.
  std::filesystem::path startup_instance_root_;
  // Applies the effective (per-instance with global fallback) Qt style /
  // QSS theme for instance_root to the live QApplication.
  void apply_effective_appearance(const std::filesystem::path& instance_root);
  // True when the constructor detected a CLI error (e.g. conflicting
  // --handle-* flags) and run() should exit immediately with
  // early_exit_code_. The QApplication ctor is not allowed to return values.
  bool early_exit_     = false;
  int early_exit_code_ = 0;
};

}  // namespace Core
