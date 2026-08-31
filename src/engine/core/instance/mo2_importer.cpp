#include "engine/core/instance/mo2_importer.h"

#include "engine/core/instance/instance_utils.h"
#include "engine/core/instance/toml_utils.h"
#include "engine/core/log/logger.h"
#include "engine/core/util/fs_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace engine {

namespace {
// MO2 game name -> GMM game_id mapping
const std::map<std::string, std::string> MO2_GAME_MAP = {
    {"Skyrim Special Edition", "SkyrimSpecialEdition"},
    {"Skyrim", "Skyrim"},
    {"Fallout 4", "Fallout4"},
    {"Fallout: New Vegas", "FalloutNewVegas"},
    {"Oblivion", "Oblivion"},
    {"Morrowind", "Morrowind"},
    {"The Witcher 3", "Witcher3"},
    {"Stardew Valley", "StardewValley"},
    {"Valheim", "Valheim"},
    {"Kerbal Space Program", "KSP"},
    {"Baldur's Gate 3", "BaldursGate3"},
    {"Cyberpunk 2077", "Cyberpunk2077"},
    {"Dragon Age: Origins", "DragonAgeOrigins"},
    {"Dragon Age 2", "DragonAge2"},
    {"Dragon Age: Inquisition", "DragonAgeInquisition"},
    {"Divinity: Original Sin 2", "DivinityOriginalSin2"},
    {"No Man's Sky", "NoMansSky"},
    {"Monster Hunter: World", "MonsterHunterWorld"},
    {"Fallout 3", "Fallout3"},
    {"Fallout: New Vegas", "FalloutNewVegas"},
    {"Starfield", "Starfield"},
    {"Satisfactory", "Satisfactory"},
    {"Subnautica", "Subnautica"},
    {"7 Days to Die", "SevenDaysToDie"},
    {"Mount & Blade II: Bannerlord", "MountAndBlade2Bannerlord"},
    {"Mount & Blade: Warband", "MountAndBladeWarband"},
    {"Terraria", "Terraria"},
    {"Rimworld", "Rimworld"},
    {"Minecraft", "Minecraft"},
    {"Detroit: Become Human", "DetroitBecomeHuman"},
    {"The Sims 4", "TheSims4"},
    {"Guild Wars 2", "GuildWars2"},
    {"World of Warcraft", "WorldOfWarcraft"},
    {"Final Fantasy XIV", "FinalFantasyXIV"},
    {"Arma 3", "Arma3"},
    {"Euro Truck Simulator 2", "EuroTruckSimulator2"},
    {"American Truck Simulator", "AmericanTruckSimulator"},
    {"The Sims 3", "TheSims3"},
    {"Kingdom Come: Deliverance", "KingdomComeDeliverance"},
    {"X4: Foundations", "X4Foundations"},
    {"MechWarrior 5: Mercenaries", "MechWarrior5Mercenaries"},
    {"Space Engineers", "SpaceEngineers"},
    {"Conan Exiles", "ConanExiles"},
    {"The Forest", "TheForest"},
    {"Sons of the Forest", "SonsOfTheForest"},
    {"Subnautica: Below Zero", "SubnauticaBelowZero"},
    {"Deep Rock Galactic", "DeepRockGalactic"},
    {"Lethal Company", "LethalCompany"},
    {"Content Warning", "ContentWarning"},
    {"Gatekeeper", "Gatekeeper"},
};

// Simple INI parser for MO2's ModOrganizer.ini
struct IniSection {
  std::map<std::string, std::string> values;
};

struct IniFile {
  std::map<std::string, IniSection> sections;

  std::string get(const std::string &section, const std::string &key,
                  const std::string &default_val = "") const {
    auto sec_it = sections.find(section);
    if (sec_it == sections.end())
      return default_val;
    auto key_it = sec_it->second.values.find(key);
    if (key_it == sec_it->second.values.end())
      return default_val;
    return key_it->second;
  }
};

// Trim whitespace from both ends of a string
std::string trim(const std::string &s) {
  auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Parse an INI file
IniFile parse_ini(const fs::path &path) {
  IniFile ini;
  std::ifstream f(path);
  if (!f)
    return ini;

  std::string current_section;
  std::string line;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#')
      continue;

    if (line[0] == '[') {
      auto end = line.find(']');
      if (end != std::string::npos) {
        current_section = trim(line.substr(1, end - 1));
      }
      continue;
    }

    auto eq = line.find('=');
    if (eq != std::string::npos) {
      std::string key = trim(line.substr(0, eq));
      std::string value = trim(line.substr(eq + 1));
      ini.sections[current_section].values[key] = value;
    }
  }
  return ini;
}

// Lowercase a string for case-insensitive comparison
std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// Count folders in a directory
int count_directories(const fs::path &dir) {
  int count = 0;
  std::error_code ec;
  if (fs::is_directory(dir, ec)) {
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      if (entry.is_directory())
        ++count;
    }
  }
  return count;
}

// Copy directory recursively, returning true on success
bool copy_directory_recursive(const fs::path &src, const fs::path &dst) {
  std::error_code ec;
  fs::copy(src, dst,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing,
           ec);
  if (ec) {
    Logger::instance().warn("Failed to copy " + src.string() + " -> " +
                            dst.string() + ": " + ec.message());
    return false;
  }
  return true;
}
} // namespace

std::string Mo2GameMap::lookup(const std::string &mo2_game_name) {
  auto it = MO2_GAME_MAP.find(mo2_game_name);
  if (it != MO2_GAME_MAP.end())
    return it->second;
  return "";
}

std::string Mo2GameMap::fuzzy_lookup(const std::string &mo2_game_name) {
  // Try exact match first
  std::string result = lookup(mo2_game_name);
  if (!result.empty())
    return result;

  // Try case-insensitive match
  std::string lower_name = to_lower(mo2_game_name);
  for (const auto &[name, id] : MO2_GAME_MAP) {
    if (to_lower(name) == lower_name)
      return id;
  }

  // Try substring match
  for (const auto &[name, id] : MO2_GAME_MAP) {
    std::string lower_map_name = to_lower(name);
    if (lower_map_name.find(lower_name) != std::string::npos ||
        lower_name.find(lower_map_name) != std::string::npos)
      return id;
  }

  return "";
}

Mo2ImportResult import_mo2_instance(const fs::path &mo2_dir,
                                    const fs::path &gmm_root,
                                    const std::string &display_name) {
  Mo2ImportResult result;

  // Validate MO2 directory
  auto ini_path = mo2_dir / "ModOrganizer.ini";
  if (!fs::exists(ini_path)) {
    result.error = "ModOrganizer.ini not found in " + mo2_dir.string();
    return result;
  }

  // Parse ModOrganizer.ini
  IniFile ini = parse_ini(ini_path);

  // Extract MO2 metadata
  std::string mo2_app_name = ini.get("General", "appName");
  std::string mo2_game_name = ini.get("General", "gameName");
  std::string mo2_game_dir = ini.get("General", "gameDirectory");
  std::string mo2_steam_appid_str = ini.get("General", "steamAppID");
  std::string mo2_profile_name = ini.get("General", "profileName");

  result.mo2_game_name = mo2_game_name;

  // Map MO2 game name to GMM game_id
  std::string game_id = Mo2GameMap::fuzzy_lookup(mo2_game_name);
  result.mapped_game_id = game_id;

  if (game_id.empty()) {
    Logger::instance().warn("MO2 game name not recognized: '" + mo2_game_name +
                            "'. Creating gameless instance.");
  }

  // Parse Steam app ID
  uint32_t steam_appid = 0;
  if (!mo2_steam_appid_str.empty()) {
    try {
      steam_appid = static_cast<uint32_t>(std::stoul(mo2_steam_appid_str));
    } catch (...) {
      Logger::instance().warn("Failed to parse steamAppID: " +
                              mo2_steam_appid_str);
    }
  }

  // Determine display name
  std::string inst_name = display_name.empty() ? mo2_app_name : display_name;
  if (inst_name.empty()) {
    inst_name = mo2_game_name.empty() ? "MO2 Import" : mo2_game_name;
  }

  // Ensure GMM root exists
  std::error_code ec;
  if (!fs::is_directory(gmm_root, ec)) {
    fs::create_directories(gmm_root, ec);
  }

  // Create instance name (unique)
  std::string sanitized = Instance::to_instance_name(inst_name);
  if (sanitized.empty())
    sanitized = "MO2 Import";
  std::string unique_name = unique_instance_name(sanitized, gmm_root);

  // Create the instance
  Instance inst = Instance::installed(unique_name, gmm_root);
  inst.info().game_id = game_id;
  inst.info().display_name = inst_name;
  inst.info().steam_appid = steam_appid;

  // Set game_dir if MO2 provided it
  if (!mo2_game_dir.empty()) {
    inst.info().game_dir = fs::path(mo2_game_dir);
  }

  if (!inst.create_directories()) {
    result.error = "Failed to create instance directories";
    return result;
  }

  // Copy mods directory
  fs::path mo2_mods = mo2_dir / "mods";
  if (fs::is_directory(mo2_mods, ec)) {
    fs::path gmm_mods = inst.path_for(InstanceKind::Mods);
    if (copy_directory_recursive(mo2_mods, gmm_mods)) {
      result.mods_imported = count_directories(gmm_mods);
    }
  }

  // Copy profiles directory
  fs::path mo2_profiles = mo2_dir / "profiles";
  if (fs::is_directory(mo2_profiles, ec)) {
    fs::path gmm_profiles = inst.path_for(InstanceKind::Profiles);
    if (copy_directory_recursive(mo2_profiles, gmm_profiles)) {
      result.profiles_imported = count_directories(gmm_profiles);
    }
  }

  // Copy downloads directory
  fs::path mo2_downloads = mo2_dir / "downloads";
  if (fs::is_directory(mo2_downloads, ec)) {
    fs::path gmm_downloads = inst.path_for(InstanceKind::Downloads);
    copy_directory_recursive(mo2_downloads, gmm_downloads);
  }

  // Copy overwrite directory
  fs::path mo2_overwrite = mo2_dir / "overwrite";
  if (fs::is_directory(mo2_overwrite, ec)) {
    fs::path gmm_overwrite = inst.path_for(InstanceKind::Overwrite);
    copy_directory_recursive(mo2_overwrite, gmm_overwrite);
  }

  // Skip webcache/ (not needed)

  // Write instance.toml
  if (!inst.write_toml()) {
    result.error = "Failed to write instance.toml";
    return result;
  }

  Logger::instance().info(
      "MO2 instance imported: " + inst_name + " (game=" + game_id +
      ", mods=" + std::to_string(result.mods_imported) +
      ", profiles=" + std::to_string(result.profiles_imported) + ")");

  result.success = true;
  result.instance = inst;
  return result;
}

} // namespace engine
