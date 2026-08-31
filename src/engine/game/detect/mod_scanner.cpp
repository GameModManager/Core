#include "engine/game/detect/mod_scanner.h"
#include "engine/core/log/logger.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/mod/meta/mod_meta.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <sstream>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace engine {

// --- Simple XML tag extraction (no libxml/Qt dependency) ---

static std::string xml_find_tag(const std::string &xml,
                                const std::string &tag) {
  auto open = "<" + tag + ">";
  auto close = "</" + tag + ">";
  auto pos = xml.find(open);
  if (pos == std::string::npos)
    return {};
  pos += open.size();
  auto end = xml.find(close, pos);
  if (end == std::string::npos)
    return {};
  auto content = xml.substr(pos, end - pos);
  auto first = content.find_first_not_of(" \t\n\r");
  if (first == std::string::npos)
    return {};
  auto last = content.find_last_not_of(" \t\n\r");
  return content.substr(first, last - first + 1);
}

static std::string read_file_text(const std::filesystem::path &path) {
  std::ifstream f(path);
  if (!f)
    return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

// --- Helpers ---

static std::vector<std::string> split_csv(const std::string &s) {
  std::vector<std::string> result;
  std::istringstream ss(s);
  std::string token;
  while (std::getline(ss, token, ',')) {
    // trim
    auto start = token.find_first_not_of(" \t");
    auto end = token.find_last_not_of(" \t");
    if (start != std::string::npos) {
      result.push_back(token.substr(start, end - start + 1));
    }
  }
  return result;
}

static bool ci_equals(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

static bool should_ignore(const std::string &name,
                          const std::vector<std::string> &ignored) {
  for (const auto &ig : ignored) {
    if (ci_equals(name, ig))
      return true;
  }
  return false;
}

// Folder timestamps for the mod list's Installation/Changed columns. MO2's
// COL_INSTALLTIME reads the mod folder's birth time (a Replace install
// recreates the folder → new time; Merge keeps it → old time preserved), and
// "Changed" is the folder's last-write time. Birth time is not in the C++
// std::filesystem API, so it needs statx(2) on Linux; when the filesystem
// doesn't report btime the install time falls back to the write time. Epoch
// seconds, 0 on error.
static void folder_timestamps(const std::filesystem::path &dir,
                              int64_t &install, int64_t &changed) {
  install = 0;
  changed = 0;

  std::error_code ec;
  auto mtime = std::filesystem::last_write_time(dir, ec);
  if (ec)
    return;
  // last_write_time runs on the filesystem clock, whose epoch is NOT the
  // Unix epoch on libstdc++ (__file_clock). Convert explicitly via the
  // standard to_sys()/from_sys() pair, or the raw duration is garbage.
  const int64_t mtime_sec = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::clock_cast<std::chrono::system_clock>(mtime)
              .time_since_epoch())
          .count());
  changed = mtime_sec;

#if defined(__linux__)
  struct statx stx;
  if (::statx(AT_FDCWD, dir.c_str(), 0, STATX_BTIME, &stx) == 0 &&
      (stx.stx_mask & STATX_BTIME)) {
    install = static_cast<int64_t>(stx.stx_btime.tv_sec);
  }
#endif
  if (install <= 0)
    install = mtime_sec;
}

// --- ModScanner ---

// Map Steam Workshop tags to internal category IDs using the
// workshop_tag_categories hook (JSON: {"lowercase_tag": category_id}).
// Tags are compared case-insensitively. Returns empty when no mapping
// is configured or no tags match.
static std::vector<int>
map_workshop_tags_to_categories(const std::vector<std::string> &tags,
                                const std::string &mapping_json) {
  std::vector<int> result;
  if (mapping_json.empty() || tags.empty())
    return result;

  try {
    auto mapping = nlohmann::json::parse(mapping_json);
    if (!mapping.is_object())
      return result;

    for (const auto &tag : tags) {
      // Lowercase the tag for case-insensitive matching
      std::string lower_tag = tag;
      std::transform(lower_tag.begin(), lower_tag.end(), lower_tag.begin(),
                     [](unsigned char c) { return std::tolower(c); });

      auto it = mapping.find(lower_tag);
      if (it != mapping.end() && it->is_number_integer()) {
        int cat_id = it->get<int>();
        // Deduplicate
        if (std::find(result.begin(), result.end(), cat_id) == result.end())
          result.push_back(cat_id);
      }
    }
  } catch (...) {
    // Malformed JSON - no categories assigned
  }
  return result;
}

// Per-game settings read from GameKnowledge hooks, shared by the directory scan
// and the single-folder scan so both classify a mod identically.
struct ScanConfig {
  std::string disable_file;
  std::vector<std::string> ignored;
  std::string metadata_file;
  std::string name_tag;
  std::string version_tag;
  bool use_xml_meta = false;
  std::string separator_suffix;
  std::string workshop_pattern;
  std::string priority_prefix_re;
  // Per-game content allow-lists (MO2's GamebryoModDataChecker analogue).
  // Both empty → no checker registered → no folder can look invalid.
  std::vector<std::string> valid_dirs;
  std::vector<std::string> valid_exts;
  // Steam Workshop tag → category mapping (JSON: {"lowercase_tag": cat_id}).
  // Empty when the game doesn't register the workshop_tag_categories hook.
  std::string workshop_tag_categories;
};

// MO2's GamebryoModDataChecker::dataLooksValid analogue: a folder has valid
// game data if it contains a recognized metadata file (managed mod), at
// least one game plugin / archive file (.esp/.esm/.esl/.bsa/.ba2) at the top
// level, OR a top-level subdirectory whose name matches a recognized data
// directory from the checker's allow-list. No allow-lists registered →
// nothing can look invalid.
static bool content_looks_valid(const ScanConfig &cfg,
                                const std::filesystem::path &entry_path) {
  if (cfg.valid_dirs.empty())
    return true;

  if (!cfg.metadata_file.empty() &&
      std::filesystem::exists(entry_path / cfg.metadata_file))
    return true;

  static const std::vector<std::string> kPluginExts = {"esp", "esm", "esl",
                                                       "bsa", "ba2"};

  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(entry_path, ec)) {
    // Check for recognized data subdirectories (MO2 GamebryoModDataChecker
    // allows mods with recognized folder names like textures/, meshes/, etc.)
    if (entry.is_directory(ec)) {
      auto dir_name = entry.path().filename().string();
      for (const auto &vd : cfg.valid_dirs)
        if (ci_equals(dir_name, vd))
          return true;
    }
    // Check for game plugin / archive files at the top level
    if (!entry.is_regular_file(ec))
      continue;
    auto dot = entry.path().filename().string().find_last_of('.');
    if (dot == std::string::npos)
      continue;
    auto ext = entry.path().filename().string().substr(dot + 1);
    for (const auto &pe : kPluginExts)
      if (ci_equals(ext, pe))
        return true;
  }
  return false;
}

static ScanConfig make_scan_config(const GameKnowledge &knowledge,
                                   const std::string &game_id) {
  ScanConfig cfg;
  cfg.disable_file = disable_mechanism_for(knowledge, game_id);
  auto ignored_csv = knowledge.get(game_id, "ignored_files", "");
  // MO2-style metadata is a meta.ini in the mod folder. Games whose engine
  // reads XML metadata from mod folders (Isaac's metadata.xml) register the
  // filename and name/version tags via hooks.
  cfg.metadata_file = knowledge.get(game_id, "metadata_file", "meta.ini");
  cfg.name_tag = knowledge.get(game_id, "metadata_name_tag", "name");
  cfg.version_tag = knowledge.get(game_id, "metadata_version_tag", "version");
  cfg.use_xml_meta =
      !cfg.metadata_file.empty() && cfg.metadata_file != "meta.ini";
  cfg.separator_suffix =
      knowledge.get(game_id, "separator_suffix", "_separator");
  cfg.workshop_pattern = knowledge.get(game_id, "workshop_id_pattern", "");
  cfg.priority_prefix_re = knowledge.get(game_id, "priority_prefix_re", "");
  // Content-validity allow-lists come from the game's support plugin, never
  // hardcoded here: mod_valid_dirs = valid top-level folder names (CSV),
  // mod_valid_exts = valid top-level file extensions (CSV, no leading dot).
  cfg.valid_dirs = split_csv(knowledge.get(game_id, "mod_valid_dirs", ""));
  cfg.valid_exts = split_csv(knowledge.get(game_id, "mod_valid_exts", ""));

  cfg.ignored = split_csv(ignored_csv);
  // Game-registered vanilla directories (e.g. Scripts/, Meshes/) that must
  // not appear as mods.  The "ignored_dirs" hook is a CSV of folder names
  // the engine should skip during directory scanning - same semantics as
  // ignored_files but scoped to subdirectories of the mods dir.
  auto ignored_dirs = split_csv(knowledge.get(game_id, "ignored_dirs", ""));
  for (auto &d : ignored_dirs) {
    if (!should_ignore(d, cfg.ignored))
      cfg.ignored.push_back(std::move(d));
  }
  // Steam Workshop tag → category mapping (optional per-game hook)
  cfg.workshop_tag_categories =
      knowledge.get(game_id, "workshop_tag_categories", "");
  // Always ignore system directories during directory scanning
  cfg.ignored.emplace_back("overwrite");
  if (!cfg.metadata_file.empty() &&
      should_ignore(cfg.metadata_file, cfg.ignored) == false) {
    cfg.ignored.push_back(cfg.metadata_file);
  }
  if (!cfg.disable_file.empty() &&
      should_ignore(cfg.disable_file, cfg.ignored) == false) {
    cfg.ignored.push_back(cfg.disable_file);
  }
  // Content-validity allow-lists drive MO2's FLAG_INVALID ("No valid game
  // data"). The P1.2 Game::Features::Registry is the override seam: any plugin
  // can register a mod_data_checker for this game (priority + replace, MO2
  // IGameFeatures - combined across all registered checkers). A registered
  // checker wins; the per-game CSV hooks (mod_valid_dirs/mod_valid_exts)
  // remain the fallback for games whose plugin still uses them (Isaac) and
  // for the scanner's own knowledge-driven tests.
  auto checker =
      Game::Features::Registry::instance().resolve_mod_data_checker(game_id);
  if (checker) {
    cfg.valid_dirs = checker->folder_names();
    cfg.valid_exts = checker->file_extensions();
  } else {
    cfg.valid_dirs = split_csv(knowledge.get(game_id, "mod_valid_dirs", ""));
    cfg.valid_exts = split_csv(knowledge.get(game_id, "mod_valid_exts", ""));
  }
  return cfg;
}

// Classify a single mod folder. Returns nullopt for ignored folders, symlink
// folders pointing into managed trees (e.g. Overwrite), and folders with no
// recognized metadata.
static std::optional<ScannedMod>
scan_entry(const std::filesystem::path &entry_path, const ScanConfig &cfg,
           const std::vector<std::filesystem::path> &ignore_symlink_targets) {
  auto folder_name = entry_path.filename().string();
  if (should_ignore(folder_name, cfg.ignored))
    return std::nullopt;

  // Skip directories that are symlinks to paths we manage (e.g. Overwrite)
  if (!ignore_symlink_targets.empty()) {
    std::error_code ec2;
    if (std::filesystem::is_symlink(entry_path, ec2)) {
      auto link_target = std::filesystem::read_symlink(entry_path, ec2);
      if (!ec2) {
        if (link_target.is_relative())
          link_target =
              std::filesystem::absolute(entry_path.parent_path() / link_target);
        auto resolved = std::filesystem::weakly_canonical(link_target, ec2);
        if (!ec2) {
          bool skip = false;
          for (const auto &ignore_root : ignore_symlink_targets) {
            auto canon_root =
                std::filesystem::weakly_canonical(ignore_root, ec2);
            if (!ec2) {
              auto r_str = resolved.string();
              auto i_str = canon_root.string();
              if (r_str.size() >= i_str.size() &&
                  r_str.compare(0, i_str.size(), i_str) == 0) {
                skip = true;
                break;
              }
            }
          }
          if (skip)
            return std::nullopt;
        }
      }
    }
  }

  ScannedMod mod;
  mod.folder_name = folder_name;

  // Extract workshop ID from folder name if pattern is configured
  if (!cfg.workshop_pattern.empty()) {
    try {
      std::regex ws_re(cfg.workshop_pattern);
      std::smatch m;
      if (std::regex_search(folder_name, m, ws_re)) {
        mod.workshop_id = std::stoll(m[1].str());
      }
    } catch (...) {
    }
  }

  // Installation/Changed for the mod list: folder birth time and last-write
  // time (MO2 COL_INSTALLTIME semantics). Set for separators too; the
  // Overwrite/MERGED pseudo-rows are synthesized elsewhere and stay 0.
  folder_timestamps(entry_path, mod.install_time, mod.changed_time);

  // Check for separator (game-specific suffix)
  if (!cfg.separator_suffix.empty() &&
      folder_name.size() > cfg.separator_suffix.size() &&
      folder_name.compare(folder_name.size() - cfg.separator_suffix.size(),
                          cfg.separator_suffix.size(),
                          cfg.separator_suffix) == 0) {
    mod.is_separator = true;

    // MO2-style separator: the folder is "<name>_separator" and the
    // display name is the folder minus the suffix (ModList::getDisplayName).
    // An optional color comes from the meta.ini [General] color key -
    // the same file MO2's setColor writes. No color means no color.
    mod.display_name =
        folder_name.substr(0, folder_name.size() - cfg.separator_suffix.size());
    auto meta_path = entry_path / "meta.ini";
    auto meta_content = read_file_text(meta_path);
    if (!meta_content.empty()) {
      engine::ModMeta meta;
      if (meta.parse(meta_content)) {
        mod.separator_color = meta.get("General", "color");
      }
    }
    mod.raw_name = folder_name;
    return mod;
  }

  // Parse metadata file
  if (cfg.use_xml_meta) {
    // XML metadata (Isaac): name/version come from the configured tags.
    // A folder without the metadata file is STILL a mod (MO2 lists every
    // folder in Mods/); it is flagged no_metadata so the UI can warn it
    // wasn't installed by the manager.
    auto metadata_path = entry_path / cfg.metadata_file;
    if (!std::filesystem::exists(metadata_path)) {
      mod.no_metadata = true;
      mod.raw_name = folder_name;
      mod.display_name = folder_name;
    } else {
      auto content = read_file_text(metadata_path);
      auto raw_name =
          content.empty() ? std::string{} : xml_find_tag(content, cfg.name_tag);
      if (raw_name.empty()) {
        mod.raw_name = folder_name;
        mod.display_name = folder_name;
      } else {
        mod.raw_name = raw_name;

        // Normalize name: strip priority prefix if configured
        if (!cfg.priority_prefix_re.empty()) {
          try {
            static const std::regex prefix_re(cfg.priority_prefix_re);
            // Extract the numeric prefix value before stripping
            std::smatch m;
            if (std::regex_search(raw_name, m, prefix_re)) {
              auto prefix_str = m.str();
              // Strip non-digits to get the number
              std::string digits;
              for (char c : prefix_str) {
                if (std::isdigit(static_cast<unsigned char>(c)))
                  digits += c;
              }
              if (!digits.empty()) {
                try {
                  mod.priority = std::stoi(digits);
                } catch (...) {
                }
              }
            }
            mod.display_name = std::regex_replace(raw_name, prefix_re, "");
          } catch (...) {
            mod.display_name = raw_name;
          }
        } else {
          mod.display_name = raw_name;
        }

        // Trim whitespace from display name
        auto first = mod.display_name.find_first_not_of(" \t");
        if (first != std::string::npos) {
          mod.display_name = mod.display_name.substr(first);
        }

        // Parse version
        mod.version = xml_find_tag(content, cfg.version_tag);
      }
    }
  } else {
    // MO2-style meta.ini: the folder name IS the mod name; the ini
    // carries the version. A legacy metadata.xml (written by older GMM
    // installs for every game) is read as a fallback so pre-fix mods
    // keep loading - it is never written for non-XML games again.
    auto meta_path = entry_path / "meta.ini";
    auto content = read_file_text(meta_path);
    if (content.empty()) {
      auto legacy_path = entry_path / "metadata.xml";
      auto legacy = read_file_text(legacy_path);
      if (!legacy.empty()) {
        mod.raw_name = folder_name;
        mod.display_name = folder_name;
        mod.version = xml_find_tag(legacy, "version");
      } else {
        // No metadata at all: still emit the folder (MO2 lists every
        // folder) and flag it so the UI warns it isn't a managed mod.
        mod.no_metadata = true;
        mod.raw_name = folder_name;
        mod.display_name = folder_name;
      }
    } else {
      engine::ModMeta meta;
      if (meta.parse(content)) {
        mod.raw_name = folder_name;
        mod.display_name = folder_name;
        mod.version = meta.get("General", "version");
        // FOMOD-installed marker: install_stage writes [fomod] choices=
        // so reinstalls can restore selections and the UI can flag the
        // mod. This is the retroactive scan too - every load re-reads
        // meta.ini, so mods installed before the marker existed are
        // picked up here if their meta.ini already has the section.
        mod.is_fomod =
            meta.has_section("fomod") && !meta.get("fomod", "choices").empty();
        // Root-override marker: when set, the mod's folder is treated as
        // the game's root directory at deploy time (files under a leading
        // Data/ folder still land in Data/; everything else goes to root).
        mod.root_override = meta.get("General", "rootOverride") == "1";
      } else {
        // Malformed meta.ini: MO2's QSettings reads it as empty, so the
        // folder keeps its defaults rather than vanishing from the list.
        mod.raw_name = folder_name;
        mod.display_name = folder_name;
      }
    }
  }

  // MO2's validated marker: [General] validated=true in the folder's
  // meta.ini - the file MO2's markValidated writes ("Ignore missing data").
  // Read for every game (an XML game's meta.ini is GMM-owned, never read by
  // the game itself) so the marker suppresses both flags below.
  if (!mod.validated) {
    auto meta_path = entry_path / "meta.ini";
    auto meta_content = read_file_text(meta_path);
    if (!meta_content.empty()) {
      engine::ModMeta meta;
      if (meta.parse(meta_content) &&
          meta.get("General", "validated") == "true") {
        mod.validated = true;
      }
    }
  }
  mod.no_metadata = mod.no_metadata && !mod.validated;
  mod.invalid_data = !mod.validated && !content_looks_valid(cfg, entry_path);

  // Check for disable sentinel
  if (!cfg.disable_file.empty()) {
    auto disable_path = entry_path / cfg.disable_file;
    mod.enabled = !std::filesystem::exists(disable_path);
  }

  // Map Steam Workshop tags to category IDs when the game provides
  // a workshop_tag_categories hook. Tags are stored in meta.ini by the
  // SteamWorkshopProvider during download; the mapping is a JSON object
  // of {lowercase_tag: category_id} registered via the plugin hook.
  if (!cfg.workshop_tag_categories.empty()) {
    auto meta_path = entry_path / "meta.ini";
    auto meta_content = read_file_text(meta_path);
    if (!meta_content.empty()) {
      engine::ModMeta meta;
      if (meta.parse(meta_content)) {
        auto tags_csv = meta.get("SteamWorkshop", "tags");
        if (!tags_csv.empty()) {
          auto tags = split_csv(tags_csv);
          mod.category_ids = map_workshop_tags_to_categories(
              tags, cfg.workshop_tag_categories);
        }
      }
    }
  }

  return mod;
}

// Shared implementation: scan the given mods_dir for mods.
static std::vector<ScannedMod>
scan_impl(const GameKnowledge &knowledge, const std::string &game_id,
          const std::filesystem::path &mods_dir,
          const std::vector<std::filesystem::path> &ignore_symlink_targets) {

  auto cfg = make_scan_config(knowledge, game_id);

  std::vector<ScannedMod> mods;
  std::error_code ec;

  if (!std::filesystem::exists(mods_dir)) {
    Logger::instance().warn("ModScanner: mods directory not found: " +
                            mods_dir.string());
    return mods;
  }

  for (const auto &entry : std::filesystem::directory_iterator(mods_dir)) {
    try {
      if (!entry.is_directory())
        continue;
    } catch (const std::filesystem::filesystem_error &) {
      continue;
    }

    // GMM-internal scratch dirs (.gmm_overlay_work, .gmm_staging, ...) are
    // never mods, wherever they land in the mods dir - they are manager
    // machinery, not user content (e.g. .gmm_overlay_work can end up here
    // as an overlay-launch artifact).
    if (entry.path().filename().string().starts_with(".gmm_"))
      continue;

    auto mod = scan_entry(entry.path(), cfg, ignore_symlink_targets);
    if (mod)
      mods.push_back(std::move(*mod));
  }

  // Sort: separators first (in insertion order), then by priority if available,
  // else alphabetical
  std::sort(mods.begin(), mods.end(),
            [](const ScannedMod &a, const ScannedMod &b) {
              if (a.is_separator != b.is_separator)
                return a.is_separator;
              if (a.priority >= 0 && b.priority >= 0)
                return a.priority < b.priority;
              if (a.priority >= 0)
                return true; // has priority, comes before alphabetical
              if (b.priority >= 0)
                return false;
              return a.display_name < b.display_name;
            });

  Logger::instance().debug("ModScanner: found " + std::to_string(mods.size()) +
                           " mods in " + mods_dir.string());

  return mods;
}

std::vector<ScannedMod> ModScanner::scan(
    const GameKnowledge &knowledge, const std::string &game_id,
    const std::filesystem::path &game_install_dir,
    const std::vector<std::filesystem::path> &ignore_symlink_targets,
    const std::filesystem::path &override_mods_dir) {

  // Workspace-93m: resolve through the single resolution point instead of
  // appending mods_subpath here. When game_mods_dir is set (instance
  // override or plugin hook) it IS the mods folder - nothing may be
  // appended to it. Empty game_mods_dir keeps the classic
  // game_install_dir/mods_subpath layout (e.g. Skyrim's Data/).
  return scan_impl(knowledge, game_id,
                   resolve_game_mods_dir(game_id, game_install_dir, knowledge,
                                         override_mods_dir.string()),
                   ignore_symlink_targets);
}

std::vector<ScannedMod> ModScanner::scan_dir(
    const GameKnowledge &knowledge, const std::string &game_id,
    const std::filesystem::path &mods_dir,
    const std::vector<std::filesystem::path> &ignore_symlink_targets) {

  return scan_impl(knowledge, game_id, mods_dir, ignore_symlink_targets);
}

std::vector<ScannedMod> ModScanner::scan_folder(
    const GameKnowledge &knowledge, const std::string &game_id,
    const std::filesystem::path &mods_dir, const std::string &folder_name,
    const std::vector<std::filesystem::path> &ignore_symlink_targets) {

  if (folder_name.empty())
    return {};
  auto cfg = make_scan_config(knowledge, game_id);
  auto mod = scan_entry(mods_dir / folder_name, cfg, ignore_symlink_targets);
  if (!mod)
    return {};
  return {std::move(*mod)};
}

bool ModScanner::disable_mod(const GameKnowledge &knowledge,
                             const std::string &game_id,
                             const std::filesystem::path &mod_folder) {
  auto disable_file = disable_mechanism_for(knowledge, game_id);

  std::ofstream f(mod_folder / disable_file);
  return f.good();
}

bool ModScanner::enable_mod(const GameKnowledge &knowledge,
                            const std::string &game_id,
                            const std::filesystem::path &mod_folder) {
  auto disable_file = disable_mechanism_for(knowledge, game_id);

  std::error_code ec;
  return std::filesystem::remove(mod_folder / disable_file, ec);
}

bool ModScanner::set_priority(const GameKnowledge &knowledge,
                              const std::string &game_id,
                              const std::filesystem::path &mod_folder,
                              int priority) {
  auto metadata_file = knowledge.get(game_id, "metadata_file", "meta.ini");
  auto name_tag = knowledge.get(game_id, "metadata_name_tag", "name");
  auto prefix_re_str = knowledge.get(game_id, "priority_prefix_re", "");
  auto format_str = knowledge.get(game_id, "priority_format", "%03d ");

  if (metadata_file.empty() || name_tag.empty())
    return false;

  auto metadata_path = mod_folder / metadata_file;
  auto content = read_file_text(metadata_path);
  if (content.empty())
    return false;

  auto old_name = xml_find_tag(content, name_tag);
  if (old_name.empty())
    return false;

  // Strip existing prefix
  std::string clean_name = old_name;
  if (!prefix_re_str.empty()) {
    try {
      static const std::regex prefix_re(prefix_re_str);
      clean_name = std::regex_replace(old_name, prefix_re, "");
    } catch (...) {
    }
  }

  // Trim whitespace
  auto first = clean_name.find_first_not_of(" \t");
  if (first != std::string::npos) {
    clean_name = clean_name.substr(first);
  }

  // Build new name with prefix
  char buf[64];
  std::snprintf(buf, sizeof(buf), format_str.c_str(), priority);
  std::string new_name = std::string(buf) + clean_name;

  // Replace tag content
  auto open_tag = "<" + name_tag + ">";
  auto close_tag = "</" + name_tag + ">";
  auto open_pos = content.find(open_tag);
  auto close_pos = content.find(close_tag);
  if (open_pos == std::string::npos || close_pos == std::string::npos)
    return false;

  content.replace(open_pos, close_pos + close_tag.size() - open_pos,
                  open_tag + new_name + close_tag);

  std::ofstream fout(metadata_path);
  if (!fout)
    return false;
  fout << content;
  return fout.good();
}

bool ModScanner::mark_validated(const std::filesystem::path &mod_folder) {
  // MO2's markValidated: persist [General] validated=true in the mod folder's
  // own meta.ini (creating the file if the folder had none). The scanner
  // reads the same key back, so the invalid/no-metadata flags stay cleared.
  auto ini_path = mod_folder / "meta.ini";
  engine::ModMeta meta;
  if (std::filesystem::exists(ini_path)) {
    auto content = read_file_text(ini_path);
    if (content.empty() || !meta.parse(content))
      return false;
  }
  meta.set("General", "validated", "true");
  return meta.save_file(ini_path);
}

} // namespace engine
