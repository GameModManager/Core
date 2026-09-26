#include "engine/core/instance/instance_utils.h"

#include "engine/core/instance/toml_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/util/fs_utils.h"
#include "engine/deploy/core.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/deploy/direct.h"
#include "engine/deploy/launch/launcher.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/game/saves/local_saves.h"
#include "platform/platform.h"

#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace engine {

namespace {
  fs::path instances_dir_override_;
}  // namespace

fs::path default_instances_dir() {
  if (!instances_dir_override_.empty())
    return instances_dir_override_;
  return safe_home_dir() / ".local/share/GameModManager/instances";
}

void set_instances_dir_override(const fs::path &dir) {
  instances_dir_override_ = dir;
}

fs::path last_instance_file_path() {
  return safe_home_dir() / ".local/share/GameModManager/last_instance";
}

std::string read_last_instance() {
  std::ifstream f(last_instance_file_path());
  std::string name;
  if (f)
    std::getline(f, name);
  return name;
}

void write_last_instance(const std::string &name) {
  std::ofstream f(last_instance_file_path());
  if (f)
    f << name << "\n";
}

std::vector<std::string> scan_instances() {
  std::vector<std::string> result;
  std::error_code ec;
  auto dir = default_instances_dir();
  if (!fs::is_directory(dir, ec))
    return result;

  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_directory())
      continue;
    auto toml = entry.path() / "instance.toml";
    if (fs::exists(toml)) {
      result.push_back(entry.path().filename().string());
    }
  }
  return result;
}

fs::path resolve_instance_path(const std::string &name_or_path) {
  if (name_or_path.empty())
    return {};

  fs::path p(name_or_path);
  if (p.is_absolute()) {
    if (fs::exists(p / "instance.toml"))
      return p;
    return {};
  }

  auto resolved = default_instances_dir() / name_or_path;
  if (fs::exists(resolved / "instance.toml"))
    return resolved;
  return {};
}

std::string unique_instance_name(const std::string &display_name,
                                 const fs::path &instances_root) {
  std::string base = Instance::to_instance_name(display_name);
  if (base.empty())
    base = "New Instance";
  const auto taken = [&](const std::string &name) {
    std::error_code ec;
    return fs::exists(instances_root / name, ec);
  };
  if (!taken(base))
    return base;
  for (int i = 2;; ++i) {
    const std::string candidate = base + " " + std::to_string(i);
    if (!taken(candidate))
      return candidate;
  }
}

std::string instance_display_name(const fs::path &instance_root) {
  if (!instance_root.empty()) {
    Instance inst = Instance::from_root(instance_root);
    if (inst.read_toml() && !inst.info().display_name.empty())
      return inst.info().display_name;
  }
  return instance_root.filename().string();
}

Instance create_instance_for_game(const DetectedGame &game,
                                  const fs::path &instances_root,
                                  const std::string &display_name) {
  // Workspace-4fu: user-chosen names are sanitized with spaces preserved;
  // empty or dot-only custom names are refused.
  // Workspace-l6w: explicit names are refused when an instance.toml already
  // exists at the target path (no auto-disambiguation for user-chosen names).
  const std::string sanitized = sanitize_directory_name(display_name);
  if (sanitized.empty()) {
    Logger::instance().error("create_instance_for_game: empty instance name for game=" +
                             game.game_id);
    return Instance::portable({});
  }
  // Refuse if an instance already exists at this exact path.
  if (fs::exists(instances_root / sanitized / "instance.toml")) {
    Logger::instance().warn("create_instance_for_game: instance already exists: " +
                            sanitized);
    return Instance::portable({});
  }
  Instance inst = Instance::installed(sanitized, instances_root);

  inst.info().game_id      = game.game_id;
  inst.info().display_name = display_name;
  inst.info().game_dir     = game.install_path;
  inst.info().steam_appid  = game.steam_appid;

  if (!inst.create_directories()) {
    Logger::instance().error("Failed to create instance directories for " + sanitized);
    return Instance::portable({});
  }
  if (!inst.write_toml()) {
    Logger::instance().error("Failed to write instance.toml for " + sanitized);
    return Instance::portable({});
  }

  Logger::instance().debug("Instance created: " + sanitized + " (game=" + game.game_id +
                           ") at " + inst.info().root.string());
  return inst;
}

Instance create_instance_for_game(const DetectedGame &game,
                                  const fs::path &instances_root) {
  // Legacy path: derive the folder name from the game name, keeping
  // to_instance_name's space->underscore folding. Auto-disambiguates with
  // " 2", " 3", etc. when the name is taken (unlike the explicit-name
  // overload which refuses duplicates).
  const std::string display_name =
      Instance::to_instance_name(game.name.empty() ? game.game_id : game.name);
  const std::string sanitized = sanitize_directory_name(display_name);
  if (sanitized.empty()) {
    Logger::instance().error("create_instance_for_game: empty instance name for game=" +
                             game.game_id);
    return Instance::portable({});
  }
  const std::string inst_name = unique_instance_name(sanitized, instances_root);
  Instance inst               = Instance::installed(inst_name, instances_root);

  inst.info().game_id      = game.game_id;
  inst.info().display_name = display_name;
  inst.info().game_dir     = game.install_path;
  inst.info().steam_appid  = game.steam_appid;

  if (!inst.create_directories()) {
    Logger::instance().error("Failed to create instance directories for " + inst_name);
    return Instance::portable({});
  }
  if (!inst.write_toml()) {
    Logger::instance().error("Failed to write instance.toml for " + inst_name);
    return Instance::portable({});
  }

  Logger::instance().debug("Instance created: " + inst_name + " (game=" + game.game_id +
                           ") at " + inst.info().root.string());
  return inst;
}

DeployConfig deploy_config_for(const fs::path &instance_root, const fs::path &game_dir,
                               const GameKnowledge &knowledge,
                               const std::string &game_id) {
  DeployConfig cfg;
  cfg.mods_dir = instance_root / "mods";
  cfg.game_dir = game_dir;
  // Deploy-target override (Workspace-6up): instance.toml "game_mods_dir"
  // points at the game's actual mods folder when it lives outside the
  // install dir. When set it replaces both game_dir and deploy_prefix as
  // the deploy root (mod files land directly in it). When empty the
  // classic layout applies unchanged: game_dir + deploy_prefix, which is
  // also how the plugin-declared mods_subpath (Skyrim "Data", Isaac
  // "mods") is honored - deploy_prefix carries it, so no separate
  // resolution here (folding mods_subpath into the root would misplace
  // root-override mods that must land in the game root).
  if (!instance_root.empty()) {
    Instance inst = Instance::from_root(instance_root);
    if (inst.read_toml())
      cfg.game_mods_dir = inst.info().game_mods_dir;
  }
  // Plugin-declared target (Workspace-otx): honored when the user did not
  // override. Absolute values (Isaac on macOS:
  // ~/Library/Application Support/...) and relative values (Isaac on
  // Linux/Windows "mods") both resolve through
  // resolve_plugin_game_mods_dir so a relative "mods" becomes
  // game_dir/mods - the SAME target the previous game_dir + deploy_prefix
  // "mods" layout produced, now via the explicit hook. Classic layout
  // otherwise - game_dir + deploy_prefix, which is also how the
  // plugin-declared mods_subpath (Skyrim "Data") is honored. Deliberately
  // NOT folding mods_subpath into the root here: that would misplace
  // root-override mods that must land in the game root.
  if (cfg.game_mods_dir.empty())
    cfg.game_mods_dir = resolve_plugin_game_mods_dir(game_id, game_dir, knowledge);
  if (!cfg.game_mods_dir.empty() && cfg.game_mods_dir == cfg.mods_dir)
    cfg.game_mods_dir.clear();  // self-referential deploy guard
  cfg.deploy_prefix = knowledge.get(game_id, "deploy_prefix", "Data");
  // game_mods_dir IS the mods folder - appending deploy_prefix would
  // double-nest (game_mods_dir/"mods"). Enforce what the DeployConfig
  // comment promises instead of relying on every consumer remembering.
  if (!cfg.game_mods_dir.empty())
    cfg.deploy_prefix.clear();
  cfg.deploy_include_mod_id =
      knowledge.get(game_id, "deploy_include_mod_id", "false") == "true";
  cfg.disable_mechanism = disable_mechanism_for(knowledge, game_id);
  cfg.case_sensitive    = knowledge.get(game_id, "case_sensitive", "true") != "false";
  if (const char *cs = std::getenv("GMM_CASE_SENSITIVE"); cs)
    cfg.case_sensitive = (std::string(cs) == "1");
  cfg.ledger_file = instance_root / ".gmm_deploy_ledger";
  // Empty game_dir -> empty backup_root (documented "caller opts out"):
  // "" / kOriginalFilesDirName would be a CWD-relative path that engine
  // file ops would happily create (Workspace-wk8).
  if (!game_dir.empty())
    cfg.backup_root = game_dir / kOriginalFilesDirName;
  return cfg;
}

std::string effective_deploy_strategy(const fs::path &instance_root,
                                      const GameKnowledge &knowledge,
                                      const std::string &game_id) {
  if (!instance_root.empty()) {
    Instance inst = Instance::from_root(instance_root);
    if (inst.read_toml() && !inst.info().deploy_strategy.empty())
      return inst.info().deploy_strategy;
  }
  return deploy_strategy_for(knowledge, game_id);
}

LaunchParams prepare_launch_params(const std::filesystem::path &instance_root,
                                   const std::filesystem::path &game_dir,
                                   const std::filesystem::path &executable,
                                   const GameKnowledge &knowledge,
                                   const std::string &game_id, uint32_t steam_appid,
                                   bool is_windows_exe) {
  LaunchPrepRequest req;
  req.instance_root  = instance_root;
  req.game_dir       = game_dir;
  req.executable     = executable;
  req.knowledge      = knowledge;
  req.game_id        = game_id;
  req.steam_appid    = steam_appid;
  req.is_windows_exe = is_windows_exe;
  return prepare_launch_params(req);
}

std::vector<std::string> split_launch_arguments(const std::string &args) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < args.size()) {
    while (i < args.size() && std::isspace(static_cast<unsigned char>(args[i])))
      ++i;
    if (i >= args.size())
      break;
    std::string token;
    if (args[i] == '"') {
      ++i;
      while (i < args.size() && args[i] != '"') {
        if (args[i] == '\\' && i + 1 < args.size())
          ++i;
        token += args[i++];
      }
      if (i < args.size())
        ++i;  // consume closing quote
    } else {
      while (i < args.size() && !std::isspace(static_cast<unsigned char>(args[i])))
        token += args[i++];
    }
    if (!token.empty())
      out.push_back(token);
  }
  return out;
}

fs::path resolve_launch_cwd(const fs::path &game_dir, const std::string &start_in) {
  if (start_in.empty())
    return {};
  fs::path p(start_in);
  if (!p.is_absolute())
    p = game_dir / p;
  return p;
}

namespace {
  // Single code-point lowercase fold. ASCII and Latin-1 (covers the umlaut-dir
  // case) fold via range arithmetic independent of the process locale;
  // anything else goes through towlower (locale-dependent, best effort).
  char32_t fold_code_point(char32_t cp) {
    if (cp >= U'A' && cp <= U'Z')
      return cp + (U'a' - U'A');
    if (cp >= U'\u00c0' && cp <= U'\u00de' && cp != U'\u00d7')
      return cp + 32;
    const wint_t lowered = std::towlower(static_cast<wint_t>(cp));
    return (lowered == WEOF) ? cp : static_cast<char32_t>(lowered);
  }

  // UTF-8-aware lowercase fold matching the GUI QString::toLower matching
  // semantics for lookup purposes: decodes to code points, folds each, and
  // re-encodes. Invalid UTF-8 bytes pass through unchanged so lookups never
  // fail to compare.
  std::string utf8_lower(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      char32_t cp           = 0;
      size_t len            = 0;
      if (c < 0x80) {
        cp  = c;
        len = 1;
      } else if ((c & 0xE0) == 0xC0) {
        cp  = c & 0x1F;
        len = 2;
      } else if ((c & 0xF0) == 0xE0) {
        cp  = c & 0x0F;
        len = 3;
      } else if ((c & 0xF8) == 0xF0) {
        cp  = c & 0x07;
        len = 4;
      } else {
        out += s[i++];
        continue;
      }
      if (i + len > s.size()) {
        out.append(s.substr(i));
        break;
      }
      bool ok = true;
      for (size_t k = 1; k < len; ++k) {
        const unsigned char d = static_cast<unsigned char>(s[i + k]);
        if ((d & 0xC0) != 0x80) {
          ok = false;
          break;
        }
        cp = (cp << 6) | (d & 0x3F);
      }
      if (!ok) {
        out += s[i++];
        continue;
      }
      cp = fold_code_point(cp);
      if (cp < 0x80) {
        out += static_cast<char>(cp);
      } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
      }
      i += len;
    }
    return out;
  }
}  // namespace

ExecutableLaunchConfig lookup_executable_launch_config(const fs::path &instance_root,
                                                       const fs::path &game_dir,
                                                       const fs::path &executable) {
  ExecutableLaunchConfig none;
  if (instance_root.empty() || game_dir.empty() || executable.empty())
    return none;

  // Canonicalize both spellings first (game_dir commonly goes through the
  // ~/.steam symlink, so a raw comparison would dead-end), then match as a
  // game-relative path. Paths escaping the game dir ("..") never match.
  std::error_code ec;
  auto canon_base     = fs::weakly_canonical(game_dir, ec);
  const fs::path base = (ec || canon_base.empty()) ? game_dir : canon_base;
  auto canon_full     = fs::weakly_canonical(executable, ec);
  if (ec || canon_full.empty())
    canon_full = executable;
  auto rel = fs::relative(canon_full, base, ec);
  if (ec || rel.empty())
    return none;
  if (rel.begin() != rel.end() && rel.begin()->string() == "..")
    return none;
  const std::string rel_lower = utf8_lower(rel.generic_string());

  auto tbl = parse_instance_toml(instance_root / "instance.toml");
  if (!tbl)
    return none;
  auto arr = (*tbl)["executables"].as_array();
  if (!arr)
    return none;
  for (const auto &elem : *arr) {
    auto entry = elem.as_table();
    if (!entry)
      continue;  // legacy plain-string entries carry no config
    auto path_value = (*entry)["path"].value<std::string>();
    if (!path_value || utf8_lower(*path_value) != rel_lower)
      continue;
    ExecutableLaunchConfig found;
    found.found = true;
    if (auto args_value = (*entry)["args"].value<std::string>())
      found.args = split_launch_arguments(*args_value);
    if (auto *env_arr = (*entry)["env"].as_array()) {
      for (const auto &v : *env_arr) {
        if (auto s = v.value<std::string>())
          found.environment.push_back(*s);
      }
    }
    if (auto cwd_value = (*entry)["cwd"].value<std::string>())
      found.cwd = resolve_launch_cwd(game_dir, *cwd_value);
    return found;
  }
  return none;
}

LaunchParams prepare_launch_params(const LaunchPrepRequest &req,
                                   const DeployProgressFn &progress) {
  LaunchParams params;
  params.executable     = req.executable;
  params.game_dir       = req.game_dir;
  params.overwrite_dir  = req.instance_root / "overwrite";
  params.steam_appid    = req.steam_appid;
  params.is_windows_exe = req.is_windows_exe;
  params.environment    = req.environment;
  params.args           = req.args;
  params.cwd            = req.cwd;

  // Per-instance Proton runner override (empty = automatic). Read from
  // instance.toml so every launch path (GUI + CLI) honors the selection.
  Instance inst = Instance::from_root(req.instance_root);
  if (inst.read_toml()) {
    params.proton_runner = inst.info().proton_runner;
  }

  // Validate inputs
  if (!fs::is_directory(req.game_dir)) {
    Logger::instance().error("prepare_launch_params: game_dir not found: " +
                             req.game_dir.string());
    return params;
  }

  // Ensure overwrite dir exists (belt-and-suspenders - instance setup should
  // have created it)
  std::error_code ec;
  fs::create_directories(params.overwrite_dir, ec);

  // Per-game deploy strategy: symlink (default) deploys mods straight into
  // game_dir and launches plain; overlayfs (explicit opt-in per game) deploys
  // into the session-wiped staging dir and launches sandboxed. A per-instance
  // "deploy_strategy" override in instance.toml (set from the UI's Deploy
  // Management selector) wins over the knowledge default. "direct" is the
  // lifecycle-object form of the symlink default: same on-disk result, but
  // routed through Deploy::Direct so deploy_all/undeploy/sync are
  // first-class.
  const std::string deploy_strategy_name =
      effective_deploy_strategy(req.instance_root, req.knowledge, req.game_id);
  const bool use_overlay       = (deploy_strategy_name == kDeployStrategyOverlayFs);
  const bool use_direct        = (deploy_strategy_name == kDeployStrategyDirect);
  params.use_overlay           = use_overlay;
  const bool overlay_supported = Deploy::Core::overlay_supported(params.overwrite_dir);
  if (use_overlay && !overlay_supported) {
    Logger::instance().warn("OverlayFS not supported, launching without overlay");
    return params;
  }

  // Deploy parameters are gathered once via deploy_config_for so the
  // launch-time deploy and the UI's "Deploy management" actions use the exact
  // same values (knowledge keys + GMM_CASE_SENSITIVE override included).
  const DeployConfig deploy_cfg =
      deploy_config_for(req.instance_root, req.game_dir, req.knowledge, req.game_id);
  // === BROKEN FEATURE - DO NOT ENABLE ===
  // Historical arm switch for the libgmm_ci_intercept.so case-insensitive
  // interposer. The shim is broken (shadows Wine's own case-insensitivity,
  // broke Pandora, 2026-08-09) and do_launch only honors ci_resolve when
  // GMM_ENABLE_BROKEN_CI_SHIM is explicitly set. Kept as inert documentation
  // of the old wiring; do not build on it.
  params.ci_resolve = !deploy_cfg.case_sensitive;

  // The engine deploy is synchronous: it returns only once the deploy tree
  // is fully populated, so every launch path chains launch on this return
  // (the GUI via a worker thread's completion signal, the CLI by plain
  // blocking).
  bool deployed = false;
  if (use_overlay) {
    // Ensure staging dir exists (fixes ENOENT when no mods have been
    // deployed yet)
    auto staging_dir = req.instance_root / ".gmm_staging";
    fs::create_directories(staging_dir, ec);
    if (ec) {
      Logger::instance().error("Failed to create staging dir: " + ec.message());
      return params;
    }
    deployed = deploy_all_enabled_mods_parallel(
        deploy_cfg.mods_dir, staging_dir, deploy_cfg.deploy_prefix,
        deploy_cfg.deploy_include_mod_id, deploy_cfg.disable_mechanism,
        deploy_cfg.case_sensitive, 0, progress);
    if (!deployed) {
      Logger::instance().warn(
          "Some mods failed to deploy to staging - continuing anyway");
    }
    params.extra_lowerdirs.push_back(staging_dir);
    Logger::instance().debug("Launch: OverlayFS staging at " + staging_dir.string());
  } else if (use_direct) {
    // "direct" strategy: the lifecycle-object form of the symlink
    // default. Deploy::Direct owns the same DeployConfig values and
    // wraps deploy_all_enabled_mods_direct, so the on-disk result is
    // identical to the symlink path below - but the strategy object is
    // what resolves the name, keeping the launch path and the UI's
    // deploy-management actions on the same class.
    Deploy::Direct::Config cfg;
    cfg.mods_dir              = deploy_cfg.mods_dir;
    cfg.game_dir              = deploy_cfg.deploy_target();
    cfg.deploy_prefix         = deploy_cfg.deploy_prefix;
    cfg.deploy_include_mod_id = deploy_cfg.deploy_include_mod_id;
    cfg.disable_mechanism     = deploy_cfg.disable_mechanism;
    cfg.case_sensitive        = deploy_cfg.case_sensitive;
    cfg.ledger_file           = deploy_cfg.ledger_file;
    cfg.backup_root           = deploy_cfg.backup_root;
    Deploy::Direct strategy(std::move(cfg));
    deployed = strategy.deploy_all(progress);
    if (!deployed) {
      Logger::instance().warn(
          "Some mods failed to deploy into game_dir - continuing anyway");
    }
    Logger::instance().debug("Launch: direct deploy into " +
                             deploy_cfg.deploy_target().string());
  } else {
    // Direct-symlink mode: deploy into game_dir. The ledger persists at
    // the instance root (outside the session-wiped .gmm_staging), so a
    // conflict-resolution owner change across sessions is detected and
    // only the changed winners are touched (O(Δ) redeploy). Originals the
    // deploy displaces are parked in <game_dir>/Original_Files, never
    // deleted.
    deployed = deploy_all_enabled_mods_direct(
        deploy_cfg.mods_dir, deploy_cfg.deploy_target(), deploy_cfg.deploy_prefix,
        deploy_cfg.deploy_include_mod_id, deploy_cfg.disable_mechanism,
        deploy_cfg.case_sensitive, deploy_cfg.ledger_file, deploy_cfg.backup_root, 0,
        progress);
    if (!deployed) {
      Logger::instance().warn(
          "Some mods failed to deploy into game_dir - continuing anyway");
    }
    Logger::instance().debug("Launch: direct-symlink deploy into " +
                             deploy_cfg.deploy_target().string());
  }

  // Per-profile local saves (P4): when enabled for a Windows game with a
  // registered local_savegames feature, rewrite the game INI to save under
  // __MO_Saves and install the profile-saves bind mount into the launch
  // namespace. Reuses MO2's GamebryoLocalSavegames prepareProfile semantics
  // (backup/restore via savepath.ini), so the game keeps isolated saves per
  // instance profile.
  //
  // Only applies when the overlay launcher is in use (a bind mount needs the
  // mount namespace) and we can resolve the game's My Games folder from the
  // platform + a registered feature. Otherwise this is a silent no-op.
  if (use_overlay && req.local_saves_enabled && req.is_windows_exe && req.platform) {
    params.bind_mount_source = std::filesystem::path();
    params.bind_mount_target = std::filesystem::path();
    const auto feature =
        Game::Features::Registry::instance().resolve_feature<LocalSavegamesFeature>(
            req.game_id);
    const std::string ini_file = feature ? feature->ini_file() : std::string();
    // Single My Games resolver: Proton games land inside the prefix,
    // native games under the host Documents dir. Empty = unresolvable.
    const auto mygames = resolve_mygames_dir(req.game_id, req.knowledge, req.platform,
                                             req.is_windows_exe);
    if (feature && !ini_file.empty() && !mygames.empty()) {
      // mygames resolution + ini name come from the knowledge hooks rather
      // than the plugin's per-instance registers; use the feature's
      // ini_file. The saves subpath is where the game reads/writes saves
      // under My Games - currently only the platform Documents dir is
      // needed for the mount path.
      auto cfg =
          resolve_local_saves(mygames, req.instance_root, "Default", ini_file, true);
      const bool applied = apply_local_saves(cfg);
      (void)applied;
      auto m                   = local_saves_mount(cfg);
      params.bind_mount_source = m.first;
      params.bind_mount_target = m.second;
      if (!m.first.empty())
        Logger::instance().debug("Launch: local saves bind " + m.first.string() +
                                 " -> " + m.second.string());
    } else {
      Logger::instance().debug(
          "Launch: local saves requested but no local_savegames feature, "
          "ini_file or resolvable My Games dir for " +
          req.game_id);
    }
  }

  return params;
}

}  // namespace engine
