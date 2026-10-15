#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace engine {

class PluginLoader;
class ManagedGames;
namespace Source {
  class Registry;
}  // namespace Source

// Schema identifier for the document build_supported_games() emits. Bump the
// trailing integer only for a breaking change; additive fields (a new key in
// an existing object, a new enumeration value in `provenance` or `source_id`)
// keep the number and are what consumers must be written to tolerate. A
// consumer should refuse a document whose `schema` it does not recognise
// rather than guess.
inline constexpr const char *kSupportedGamesSchema = "gmm.supported-games/1";

// Every game GMM can act on, grouped per source, as JSON.
//
// Derived from what the loaded plugins actually registered
// (PluginLoader::game_plugins + the per-game `download_sources` knowledge hook)
// unioned with the user's managed_games.json. Nothing here is hand-maintained:
// adding a game means shipping a plugin that calls register_identity.
//
// `PluginLoader` supplies the authoritative (plugin) half; `ManagedGames`
// supplies the user's own additions. Pass an empty ManagedGames to get the
// plugin half alone. `registry` is the Source::Registry whose providers name
// the sources GMM can actually act on - a declared source name resolves to a
// provider id through it, and names with no provider come back with a null
// `source_id`. An empty registry therefore downgrades every declared source to
// `source_id: null`, which is the honest answer for a caller that has not
// loaded the providers.
//
// Qt-free by construction: this is engine code, so both a headless query and
// the GUI (when nxmproxy is installed) can call it in-process and get the same
// document the CLI prints.
[[nodiscard]] nlohmann::json build_supported_games(const PluginLoader &plugins,
                                                   const ManagedGames &managed,
                                                   const Source::Registry &registry);

}  // namespace engine
