#pragma once

// Game match validator - blocks pack installs targeting the wrong game.
//
// Before any install (fresh or append), the pack's game (manifest.json
// `info.gmmGameId`, surfaced as Pack::PackManifest::game_id) must match the
// target instance's game (instance.toml `game_id`, i.e.
// Instance::Info::game_id). Both ids are normalized to GMM's canonical game
// catalog (case/whitespace-insensitive, known aliases resolved) before
// comparison, so "SkyrimSE" and "skyrimspecialedition" compare equal while
// "skyrimspecialedition" and "fallout4" do not.
//
// Engine layer - Qt-free (only <string>).

#include <string>

namespace engine::Install
{

// Normalizes a game id to its canonical catalog form: trimmed, lowercased,
// known aliases resolved ("skyrimse" -> "skyrimspecialedition",
// "newvegas" -> "falloutnv", ...). Unknown ids pass through lowercased so a
// future game still compares equal to itself. Empty input stays empty.
[[nodiscard]] std::string normalize_game_id(const std::string& game_id);

// Outcome of validate_game_match: matches == true means install may
// proceed; otherwise error explains why (for the UI to show verbatim).
struct GameMatchResult
{
  bool matches = false;
  std::string error;  // empty when matches
};

// True when the pack's game and the instance's game are the same catalog
// entry. Either side empty, unknown-vs-known, or different games all report
// matches == false with a clear error naming both sides.
[[nodiscard]] GameMatchResult validate_game_match(const std::string& pack_game_id,
                                                  const std::string& instance_game_id);

}  // namespace engine::Install
