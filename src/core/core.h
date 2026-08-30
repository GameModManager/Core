#pragma once

#include <memory>
#include <string>

#include <QApplication>
#include <QString>

#include "cli/command_line.h"

namespace engine {
class Platform;
class PluginLoader;
class ThemeManager;
class StyleManager;
class ManagedGames;
class MultiProcess;
} // namespace engine

namespace Core {

class Application {
public:
  Application(int &argc, char **argv);
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
};

} // namespace Core
