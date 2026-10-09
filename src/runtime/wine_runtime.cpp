#include "runtime/wine_runtime.h"

#include "platform/platform.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

std::filesystem::path WineRuntime::find_wine_binary() const {
  // $WINE is a Wine convention rather than an OS path, so the override stays
  // here. Discovery below it - the PATH scan and the per-OS candidate
  // locations - belongs to the platform layer.
  if (const auto *wine_env = std::getenv("WINE"); wine_env && wine_env[0] != '\0') {
    const auto override_path = std::filesystem::path(wine_env);
    if (std::filesystem::exists(override_path))
      return override_path;
  }
  return engine::find_wine();
}

bool WineRuntime::launch(const std::filesystem::path &executable,
                         const std::filesystem::path & /*game_dir*/,
                         uint32_t /*steam_appid*/, const std::vector<std::string> &args,
                         const std::filesystem::path &cwd) {
  auto wine = find_wine_binary();
  if (wine.empty())
    return false;
  if (!std::filesystem::exists(executable))
    return false;

  // Standalone Wine path (not used by the engine launch flow, which prefers
  // Proton for .exe launches). Best-effort args/cwd support via the shell.
  std::string cmd;
  if (!cwd.empty())
    cmd = "cd \"" + cwd.string() + "\" && ";
  cmd += "\"" + wine.string() + "\" \"" + executable.string() + "\"";
  for (const auto &a : args)
    cmd += " \"" + a + "\"";
  cmd += " &";
  return std::system(cmd.c_str()) == 0;
}

bool WineRuntime::is_available() const {
  return !find_wine_binary().empty();
}

}  // namespace engine
