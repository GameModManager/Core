#include "runtime/runtime.h"

#include "platform/platform.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// --- NativeRuntime ---

bool NativeRuntime::launch(const std::filesystem::path &executable,
                           const std::filesystem::path &game_dir,
                           uint32_t /*steam_appid*/,
                           const std::vector<std::string> &args,
                           const std::filesystem::path &cwd) {
  if (!std::filesystem::exists(executable))
    return false;

  // Ensure the file is executable.
  auto st    = std::filesystem::status(executable);
  auto perms = st.permissions();
  if ((perms & std::filesystem::perms::owner_exec) == std::filesystem::perms::none) {
    std::error_code ec;
    std::filesystem::permissions(executable,
                                 perms | std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 ec);
  }

  // argv[0] is the executable, then the configured args.
  std::vector<std::string> argv;
  argv.reserve(args.size() + 1);
  argv.push_back(executable.string());
  for (const auto &a : args)
    argv.push_back(a);

  // The working directory is cwd when set, else the game dir, so relative
  // paths inside the executable resolve the way the instance expects.
  const auto work_dir = cwd.empty() ? game_dir : cwd;

  // shell_fallback: a game installed as a script with no shebang still starts,
  // which is what the old execvp-then-/bin/sh fallback bought.
  last_pid_ =
      spawn_detached_process(executable, argv, work_dir, /*shell_fallback=*/true);
  return last_pid_ > 0;
}

bool NativeRuntime::is_available() const {
  return true;
}

// --- ProtonRuntime ---

ProtonRuntime::ProtonRuntime(const Platform *platform) : platform_(platform) {}

bool ProtonRuntime::launch(const std::filesystem::path &executable,
                           const std::filesystem::path &game_dir, uint32_t steam_appid,
                           const std::vector<std::string> &args,
                           const std::filesystem::path &cwd) {
  if (!std::filesystem::exists(executable))
    return false;
  if (!platform_)
    return false;

  auto proton = find_proton_binary(platform_, steam_appid, runner_override_);
  if (proton.empty())
    return false;

  // Gate, not a second apply: the launch choke point has already put
  // proton_environment_defaults() into this process (it is the side that can
  // honour the launch Environment field), and doing it here as well is what
  // overwrote the user's value for the same key. What is left of the check is
  // the question it was really asking - can Proton be given a prefix at all -
  // and that is answered by whether the defaults come back empty.
  if (proton_environment_defaults(platform_, game_dir, steam_appid).empty())
    return false;

  // proton waitforexitandrun <exe> <args...>
  //
  // The runner is executed by its FULL path but reports itself to the game by
  // its bare filename, which is why argv[0] and the executable are separate
  // arguments here. argv[0] is built from filename() and copied into the string
  // list, because filename() returns a temporary whose c_str() would dangle
  // once the expression ends (Workspace-0y6g); the adaptor then copies every
  // argument into its own argv before forking, so that lifetime rule holds by
  // construction.
  std::vector<std::string> argv;
  argv.reserve(args.size() + 3);
  argv.push_back(proton.filename().string());
  argv.emplace_back("waitforexitandrun");
  argv.push_back(executable.string());
  for (const auto &a : args)
    argv.push_back(a);

  const auto work_dir = cwd.empty() ? game_dir : cwd;

  // No shell fallback: a missing or broken proton runner is a real failure the
  // caller must see as one, not something to paper over by re-running the
  // runner script through /bin/sh.
  last_pid_ = spawn_detached_process(proton, argv, work_dir, /*shell_fallback=*/false);
  return last_pid_ > 0;
}

// --- Static helpers ---

std::filesystem::path
ProtonRuntime::find_proton_binary(const Platform *platform, uint32_t steam_appid,
                                  const std::string &runner_override) {
  if (!platform)
    return {};

  if (!runner_override.empty()) {
    auto named = platform->find_proton_named(runner_override);
    if (!named.empty())
      return named;
  }

  if (steam_appid > 0) {
    auto proton = platform->find_proton_for_game(steam_appid);
    if (!proton.empty())
      return proton;
  }
  return platform->find_proton();
}

std::vector<std::pair<std::string, std::string>>
ProtonRuntime::proton_environment_defaults(const Platform *platform,
                                           const std::filesystem::path &game_dir,
                                           uint32_t steam_appid) {
  if (!platform)
    return {};

  auto steam_root = platform->find_steam_root();
  if (steam_root.empty())
    return {};

  auto compat_data = platform->resolve_proton_prefix(steam_appid);
  if (compat_data.empty())
    return {};

  // Build library paths - all Steam library folders
  auto libs = platform->steam_library_paths();
  std::string library_paths;
  for (const auto &lib : libs) {
    if (!library_paths.empty())
      library_paths += ":";
    library_paths += lib.string();
  }
  if (library_paths.empty()) {
    library_paths = steam_root.string();
  }

  return {
      {"STEAM_COMPAT_DATA_PATH", compat_data.string()},
      {"STEAM_COMPAT_CLIENT_INSTALL_PATH", steam_root.string()},
      {"STEAM_COMPAT_INSTALL_PATH", game_dir.string()},
      {"STEAM_COMPAT_APP_ID", std::to_string(steam_appid)},
      {"STEAM_COMPAT_LIBRARY_PATHS", library_paths},
  };
}

bool ProtonRuntime::is_available() const {
  return platform_ && !find_proton_binary(platform_, 0, runner_override_).empty();
}

}  // namespace engine
