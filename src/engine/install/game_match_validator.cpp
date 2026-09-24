#include "engine/install/game_match_validator.h"

#include <cctype>
#include <unordered_map>

namespace engine::Install {

namespace {

  // Alias -> canonical catalog id. Mirrors the MO2 GameShortName aliases in
  // source/nxm/nxm_router.cpp but resolves to GMM's long canonical ids
  // ("skyrimspecialedition", not "skyrimse"). Keys are lowercase; input is
  // lowercased before lookup.
  const std::unordered_map<std::string, std::string> &aliases() {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"morrowind", "morrowind"},
        {"oblivion", "oblivion"},
        {"skyrim", "skyrim"},
        {"skyrimse", "skyrimspecialedition"},
        {"skyrimspecialedition", "skyrimspecialedition"},
        {"enderal", "enderal"},
        {"enderalse", "enderalse"},
        {"enderalspecialedition", "enderalse"},
        {"fallout3", "fallout3"},
        {"fallout4", "fallout4"},
        {"falloutnv", "falloutnv"},
        {"falloutnewvegas", "falloutnv"},
        {"newvegas", "falloutnv"},
        {"starfield", "starfield"},
    };
    return kMap;
  }

  std::string trim(const std::string &s) {
    std::size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
      ++first;
    }
    std::size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
      --last;
    }
    return s.substr(first, last - first);
  }

}  // namespace

std::string normalize_game_id(const std::string &game_id) {
  std::string lowered;
  lowered.reserve(game_id.size());
  for (char c : trim(game_id)) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered.empty())
    return lowered;
  const auto &map = aliases();
  auto it         = map.find(lowered);
  return it != map.end() ? it->second : lowered;
}

GameMatchResult validate_game_match(const std::string &pack_game_id,
                                    const std::string &instance_game_id) {
  if (pack_game_id.empty() || trim(pack_game_id).empty()) {
    return {false, "Pack does not declare a game (missing info.gmmGameId); "
                   "cannot verify it matches this instance."};
  }
  if (instance_game_id.empty() || trim(instance_game_id).empty()) {
    return {false, "Instance has no game configured; cannot verify the "
                   "pack matches it."};
  }
  const std::string pack     = normalize_game_id(pack_game_id);
  const std::string instance = normalize_game_id(instance_game_id);
  if (pack == instance)
    return {true, {}};
  return {false, "Game mismatch: pack targets '" + pack_game_id +
                     "' but the instance is '" + instance_game_id +
                     "'. Installing mods for the wrong game would break "
                     "the instance."};
}

}  // namespace engine::Install
