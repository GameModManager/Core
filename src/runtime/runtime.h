#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace engine {

class Platform;

class Runtime {
public:
  virtual ~Runtime() = default;
  // Launch `executable` with `args` appended to argv (empty = none) and the
  // process cwd set to `cwd` when non-empty (empty = game_dir). Relative
  // cwd values are resolved against game_dir by the caller before this is
  // reached (the engine normalizes in do_launch).
  virtual bool launch(const std::filesystem::path &executable,
                      const std::filesystem::path &game_dir, uint32_t steam_appid = 0,
                      const std::vector<std::string> &args = {},
                      const std::filesystem::path &cwd     = {}) = 0;
  virtual bool is_available() const                              = 0;
  virtual std::string name() const                               = 0;
  // PID of the last launched process, or -1 if unknown / not applicable
  virtual int64_t last_pid() const { return -1; }
};

class NativeRuntime : public Runtime {
public:
  bool launch(const std::filesystem::path &executable,
              const std::filesystem::path &game_dir, uint32_t steam_appid = 0,
              const std::vector<std::string> &args = {},
              const std::filesystem::path &cwd     = {}) override;
  bool is_available() const override;
  std::string name() const override { return "native"; }
  int64_t last_pid() const override { return last_pid_; }

private:
  int64_t last_pid_ = -1;
};

class ProtonRuntime : public Runtime {
public:
  explicit ProtonRuntime(const Platform *platform);

  bool launch(const std::filesystem::path &executable,
              const std::filesystem::path &game_dir, uint32_t steam_appid = 0,
              const std::vector<std::string> &args = {},
              const std::filesystem::path &cwd     = {}) override;
  bool is_available() const override;
  std::string name() const override { return "proton"; }
  int64_t last_pid() const override { return last_pid_; }

  // Pin this runtime to a specific Proton runner (display name or absolute
  // path to a `proton` script). Empty = automatic (Steam per-game override,
  // then latest installed Proton).
  void set_runner_override(const std::string &runner) { runner_override_ = runner; }
  [[nodiscard]] const std::string &runner_override() const { return runner_override_; }

  // Find the Proton binary for a game (respects per-game compat tool
  // override). Returns empty path if none found. All discovery goes through
  // `platform`. `runner_override`, when non-empty, wins over every
  // automatic resolution.
  static std::filesystem::path
  find_proton_binary(const Platform *platform, uint32_t steam_appid = 0,
                     const std::string &runner_override = {});

  // The STEAM_COMPAT_* name/value pairs Proton needs for `steam_appid`, worked
  // out from `platform` and `game_dir`. Empty when the platform resolves no
  // Steam root or no prefix: there is then nothing to hand Proton, and the
  // caller launches without them rather than with half a set.
  //
  // Returned, not applied. Setting them is the launch choke point's job
  // (apply_proton_env_defaults in engine/deploy/launch/launcher.h), because
  // that is the only side that knows which of them the user set on purpose -
  // and a value the user chose must survive. Applying them here, as this
  // function used to, overwrote exactly those.
  static std::vector<std::pair<std::string, std::string>>
  proton_environment_defaults(const Platform *platform,
                              const std::filesystem::path &game_dir,
                              uint32_t steam_appid);

private:
  const Platform *platform_;
  std::string runner_override_;
  int64_t last_pid_ = -1;
};

}  // namespace engine
