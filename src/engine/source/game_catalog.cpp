#include "engine/source/game_catalog.h"

#include "engine/game/registry/game_knowledge.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/interface.h"
#include "engine/source/managed_games.h"
#include "engine/source/registry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace engine {
namespace {

  // Letters + digits, lowercased. Same rule (and same intent) as the
  // normalised() helper in ui/widgets/status_bar.cpp, which already matches a
  // plugin's declared `download_sources` name against a provider's
  // display_name()/source_type(). Kept ASCII on purpose: every source name GMM
  // ships is ASCII, and a locale-dependent isalnum here would make the document
  // machine-dependent.
  std::string normalize(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
      if (std::isalnum(c))
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
  }

  // The provider a plugin's declared source name means, or null when GMM has no
  // provider under either spelling. A null result is reported as a null
  // `source_id` rather than a guessed one - a wrong id would send a consumer to
  // a source that does not exist.
  const Source::Interface *provider_for(const Source::Registry &registry,
                                        const std::string &declared_name) {
    const std::string key = normalize(declared_name);
    for (const auto *provider : registry.providers()) {
      if (normalize(provider->display_name()) == key ||
          normalize(provider->source_type()) == key)
        return provider;
    }
    return nullptr;
  }

  // null rather than absent: a consumer reads one shape, not two, so an
  // unknown identifier is an explicit null and not a missing key.
  nlohmann::json maybe(const std::string &value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
  }

  nlohmann::json maybe(uint32_t value) {
    return value == 0 ? nlohmann::json(nullptr) : nlohmann::json(value);
  }

  void split_csv(const std::string &csv, std::vector<std::string> &out) {
    size_t start = 0;
    while (start <= csv.size()) {
      const size_t comma     = csv.find(',', start);
      const std::string part = csv.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
      const auto first = part.find_first_not_of(" \t");
      if (first != std::string::npos) {
        const auto last = part.find_last_not_of(" \t");
        out.push_back(part.substr(first, last - first + 1));
      }
      if (comma == std::string::npos)
        break;
      start = comma + 1;
    }
  }

  // One source offered for one game. `id` is the engine source id GMM can act
  // on (empty when unresolvable); `name` is what the plugin declared or what the
  // provider calls itself; `identifier` is the per-game handle GMM routes with
  // (the nxm:// domain, a Steam appid).
  struct SourceEntry {
    std::string id;
    std::string name;
    std::string provenance;
    std::string identifier;
    std::string website_url;

    std::vector<SourceEntry> sources;

    void add(const SourceEntry &entry) { sources.push_back(entry); }

    // The same source can be declared twice for one game: the identity hook
    // already gave us a handle, and the download_sources list names it again.
    // That is one fact, so it is reported once. A candidate is a duplicate when
    // it names the same known source id and either claims no handle or claims
    // the same one; when the id could not be resolved the display name decides.
    bool already(const SourceEntry &candidate) const {
      for (const auto &existing : sources) {
        if (!existing.id.empty() && existing.id == candidate.id &&
            (candidate.identifier.empty() ||
             candidate.identifier == existing.identifier))
          return true;
        if (existing.id.empty() &&
            normalize(existing.name) == normalize(candidate.name))
          return true;
      }
      return false;
    }
  };

  struct GameEntry {
    std::string name;
    std::string provenance;
    std::string plugin;
    uint32_t steam_appid = 0;
    std::string nexus_domain;
    std::string gog_id;
    std::string epic_namespace;
    SourceEntry source_list;
  };

}  // namespace

nlohmann::json build_supported_games(const PluginLoader &plugins,
                                     const ManagedGames &managed,
                                     const Source::Registry &registry) {
  // Keyed by game_id so the document is byte-stable across machines: plugin
  // load order is a filesystem directory walk, which is not stable, and a
  // consumer diffing two hosts' output needs it to be.
  std::map<std::string, GameEntry> games;

  for (const auto &plugin : plugins.game_plugins()) {
    auto &game          = games[plugin.game_id];
    game.name           = plugin.game_display_name;
    game.provenance     = "plugin";
    game.plugin         = std::filesystem::path(plugin.path).filename().string();
    game.steam_appid    = plugin.steam_appid;
    game.nexus_domain   = plugin.nexus_domain;
    game.gog_id         = plugin.gog_id;
    game.epic_namespace = plugin.epic_namespace;
    if (game.name.empty())
      game.name = plugin.game_id;

    // The two sources GMM can route without the plugin naming them: a Nexus
    // domain is the nxm:// key the whole download path is built on, and a
    // Steam appid is what the Workshop client fetches against.
    if (!plugin.nexus_domain.empty()) {
      const auto *provider = registry.provider_for("nexus");
      game.source_list.add({provider ? provider->source_type() : "nexus",
                            provider ? provider->display_name() : "nexus",
                            "plugin",
                            plugin.nexus_domain,
                            {}});
    }
    if (plugin.steam_appid > 0) {
      const auto *provider = registry.provider_for("steam");
      game.source_list.add({provider ? provider->source_type() : "steam",
                            provider ? provider->display_name() : "steam",
                            "plugin",
                            std::to_string(plugin.steam_appid),
                            {}});
    }

    // Everything else the plugin declared by name. Declared, not inferred:
    // if the plugin did not list a source, GMM does not offer it.
    std::vector<std::string> declared;
    split_csv(plugins.knowledge().get(plugin.game_id, "download_sources", ""),
              declared);
    for (const auto &name : declared) {
      const auto *provider = provider_for(registry, name);
      const SourceEntry candidate{
          provider ? provider->source_type() : std::string(), name, "plugin", {}, {}};
      if (game.source_list.already(candidate))
        continue;
      game.source_list.add(candidate);
    }
  }

  // The user's own additions from managed_games.json. Merged into a plugin
  // game when the ids collide, so a user can add a source GMM does not ship
  // for a game a plugin does know - which is exactly what the Sources tab
  // writes. Provenance stays "user" per source: the fact came from the user's
  // file, not from the plugin.
  for (const auto &entry : managed.entries()) {
    auto &game = games[entry.game_id];
    if (game.provenance.empty()) {
      game.provenance = "user";
      game.name       = entry.game_id;
    }
    for (const auto &source : entry.sources) {
      const auto *provider = registry.provider_for(source.source_id);
      const SourceEntry candidate{
          source.source_id, provider ? provider->display_name() : source.source_id,
          "user", source.nexus_domain, source.website_url};
      if (game.source_list.already(candidate))
        continue;
      game.source_list.add(candidate);
    }
  }

  nlohmann::json doc;
  doc["schema"]             = kSupportedGamesSchema;
  nlohmann::json games_json = nlohmann::json::array();
  for (const auto &[game_id, game] : games) {
    nlohmann::json entry;
    entry["game_id"]    = game_id;
    entry["name"]       = game.name;
    entry["provenance"] = game.provenance;
    if (!game.plugin.empty())
      entry["plugin"] = game.plugin;
    entry["identifiers"]   = {{"steam_appid", maybe(game.steam_appid)},
                              {"nexus_domain", maybe(game.nexus_domain)},
                              {"gog_id", maybe(game.gog_id)},
                              {"epic_namespace", maybe(game.epic_namespace)}};
    nlohmann::json sources = nlohmann::json::array();
    for (const auto &source : game.source_list.sources) {
      nlohmann::json item;
      item["source_id"]  = maybe(source.id);
      item["name"]       = source.name;
      item["provenance"] = source.provenance;
      item["identifier"] = maybe(source.identifier);
      if (!source.website_url.empty())
        item["website_url"] = source.website_url;
      sources.push_back(item);
    }
    entry["sources"] = std::move(sources);
    games_json.push_back(std::move(entry));
  }
  doc["games"] = std::move(games_json);
  return doc;
}

}  // namespace engine