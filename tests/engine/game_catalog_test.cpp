// The supported-games catalog: the document `gamemodmanager --list-games`
// prints, and the contract the nxmproxy integration and the conditional GUI
// both read.
//
// What is under test is the DERIVATION, not the values. Every expected value
// below is read back out of the PluginLoader / ManagedGames the fixture built,
// so a catalog that hardcoded a game, a nexus domain or a source id could not
// satisfy these cases. The two cases that cannot be read back - a declared
// source with no provider behind it, and a game nobody knows - assert absence
// rather than a literal.
//
//   - a plugin-registered game is reported with the sources its identity and
//     its `download_sources` hook declared, provenance "plugin"
//   - a game only managed_games.json knows is reported, provenance "user"
//   - a game neither the plugins nor the user file knows is absent
//   - a declared source GMM has no provider for is reported with a null
//     source_id rather than a guessed one
//   - the whole document equals a checked-in sample
//
// Hermetic: no QSettings, no network, no real plugin .so. The registry gets the
// real providers (their constructors are lazy and open nothing) so the id /
// display-name resolution runs against the same vocabulary the app uses.
#include "engine/source/game_catalog.h"

#include "engine/plugin_host/plugin_loader.h"
#include "engine/source/loverslab/provider.h"
#include "engine/source/managed_games.h"
#include "engine/source/modpub/provider.h"
#include "engine/source/nexus/provider.h"
#include "engine/source/registry.h"
#include "engine/source/steam/provider.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path temp_root() {
  static const fs::path root = fs::temp_directory_path() / "gmm-game-catalog-test";
  fs::create_directories(root);
  return root;
}

void register_providers() {
  auto &registry = engine::Source::Registry::instance();
  registry.register_provider(std::make_unique<engine::Source::Nexus::Provider>());
  registry.register_provider(std::make_unique<engine::Source::LoversLab::Provider>());
  registry.register_provider(std::make_unique<engine::Source::ModPub::Provider>());
  // The Workshop client is opened lazily by ensure_client(); the path is never
  // touched from here.
  registry.register_provider(std::make_unique<engine::Source::Steam::Provider>(
      (temp_root() / "workshop_cache.db").string()));
}

// The plugin half, shaped exactly as the ABI registration callbacks populate
// it: PluginInfo from register_identity/register_game, and the
// `download_sources` hook from register_hook. PluginLoader is neither copyable
// nor movable, so it is filled in place.
//
// The ids here are DELIBERATELY not any game's real ones. Using
// "skyrimspecialedition" would make a catalog that hardcoded a known domain
// pass this test, which is the exact failure the derivation cases exist to
// catch - an earlier draft of this file did use the real values and its
// hardcode control stayed green.
void fill_plugins(engine::PluginLoader &loader) {
  engine::PluginInfo alpha;
  alpha.path              = "/opt/gmm/plugins/TestGameAlpha.so";
  alpha.game_id           = "TestGameAlpha";
  alpha.game_display_name = "Test Game Alpha";
  alpha.game_support      = true;
  alpha.nexus_domain      = "testalpha";
  alpha.steam_appid       = 4242;
  loader.add_loaded_plugin(alpha);
  loader.knowledge().set("TestGameAlpha", "download_sources", "Nexus Mods,LoversLab");

  // A game whose only declared source is one GMM ships no provider for.
  engine::PluginInfo exotic;
  exotic.path              = "/opt/gmm/plugins/TestGameBeta.so";
  exotic.game_id           = "TestGameBeta";
  exotic.game_display_name = "Test Game Beta";
  exotic.game_support      = true;
  loader.add_loaded_plugin(exotic);
  loader.knowledge().set("TestGameBeta", "download_sources", "ModDB Direct");

  // A tool plugin: it has a game_id but never registered game support, so it
  // must not show up as a supported game - even though it carries a domain.
  engine::PluginInfo tool;
  tool.path         = "/opt/gmm/plugins/TestToolPlugin.so";
  tool.game_id      = "TestToolPlugin";
  tool.game_support = false;
  tool.nexus_domain = "testtooldomain";
  loader.add_loaded_plugin(tool);
}

// The user half: a managed_games.json written into the test's own temp dir and
// read back through the real loader.
engine::ManagedGames make_managed() {
  const fs::path json = temp_root() / "managed_games.json";
  std::ofstream out(json);
  out << R"({
    "UserOnlyGame": {"sources": [{"source_id": "modpub",
                                  "website_url": "mod.pub",
                                  "nexus_domain": "user-only-slug"}]}
  })";
  out.close();
  engine::ManagedGames managed(json);
  managed.load();
  return managed;
}

nlohmann::json build(const engine::PluginLoader &loader,
                     const engine::ManagedGames &managed) {
  return engine::build_supported_games(loader, managed,
                                       engine::Source::Registry::instance());
}

const nlohmann::json &game(const nlohmann::json &doc, const std::string &game_id) {
  for (const auto &entry : doc.at("games")) {
    if (entry.at("game_id") == game_id)
      return entry;
  }
  throw std::runtime_error("no such game in the document: " + game_id);
}

bool has_game(const nlohmann::json &doc, const std::string &game_id) {
  for (const auto &entry : doc.at("games")) {
    if (entry.at("game_id") == game_id)
      return true;
  }
  return false;
}

const nlohmann::json *source_for(const nlohmann::json &game_entry,
                                 const std::string &source_id) {
  for (const auto &source : game_entry.at("sources")) {
    if (source.at("source_id") == source_id)
      return &source;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("supported games: a plugin game carries the sources it registered",
          "[game_catalog]") {
  register_providers();
  engine::PluginLoader loader;
  fill_plugins(loader);
  const auto managed = make_managed();
  const auto doc     = build(loader, managed);

  REQUIRE(doc.at("schema") == engine::kSupportedGamesSchema);

  // Expected values are read back off the plugin registration, never written
  // out as literals: a catalog that hardcoded them could not pass this.
  // plugins() hands back the loader's own vector; game_plugins() would hand
  // back a temporary whose .front() reference dies with the full expression.
  const auto &plugin_list = loader.plugins();
  const auto &alpha       = plugin_list.front();
  REQUIRE(alpha.game_support);
  const auto &entry = game(doc, alpha.game_id);
  CHECK(entry.at("name") == alpha.game_display_name);
  CHECK(entry.at("provenance") == "plugin");
  CHECK(entry.at("plugin") == "TestGameAlpha.so");
  CHECK(entry.at("identifiers").at("nexus_domain") == alpha.nexus_domain);
  CHECK(entry.at("identifiers").at("steam_appid") == alpha.steam_appid);

  // The nexus row carries the nxm:// key, the steam row the appid, and the
  // hook-declared LoversLab row is matched to the provider GMM ships for it.
  const auto *nexus = source_for(entry, "nexus");
  REQUIRE(nexus != nullptr);
  CHECK(nexus->at("identifier") == alpha.nexus_domain);
  CHECK(nexus->at("provenance") == "plugin");

  const auto *steam = source_for(entry, "steam");
  REQUIRE(steam != nullptr);
  CHECK(steam->at("identifier") == std::to_string(alpha.steam_appid));

  const auto *loverslab = source_for(entry, "loverslab");
  REQUIRE(loverslab != nullptr);
  CHECK(loverslab->at("name") == "LoversLab");
  CHECK(loverslab->at("provenance") == "plugin");
  CHECK(loverslab->at("identifier").is_null());

  // "Nexus Mods" was declared by the hook AND implied by the identity, so it
  // must be one row, not two.
  REQUIRE(entry.at("sources").size() == 3);
}

TEST_CASE("supported games: a game only the user file knows is user provenance",
          "[game_catalog]") {
  register_providers();
  engine::PluginLoader loader;
  fill_plugins(loader);
  const auto managed = make_managed();
  const auto doc     = build(loader, managed);

  const auto &entry = game(doc, "UserOnlyGame");
  CHECK(entry.at("provenance") == "user");
  CHECK(entry.at("name") == "UserOnlyGame");
  CHECK_FALSE(entry.contains("plugin"));
  CHECK(entry.at("identifiers").at("nexus_domain").is_null());

  const auto *modpub = source_for(entry, "modpub");
  REQUIRE(modpub != nullptr);
  CHECK(modpub->at("provenance") == "user");
  CHECK(modpub->at("identifier") == "user-only-slug");
  CHECK(modpub->at("website_url") == "mod.pub");
}

TEST_CASE("supported games: a game nobody registered is absent", "[game_catalog]") {
  register_providers();
  engine::PluginLoader loader;
  fill_plugins(loader);
  const auto managed = make_managed();
  const auto doc     = build(loader, managed);

  // Nobody declared this one. Advertising it would be exactly the drift this
  // document exists to prevent.
  CHECK_FALSE(has_game(doc, "Fallout4"));
  // A tool plugin carries a nexus_domain but never registered game support, so
  // it is not a game GMM can handle either - and its domain must not leak into
  // any source row.
  CHECK_FALSE(has_game(doc, "TestToolPlugin"));
  for (const auto &entry : doc.at("games")) {
    for (const auto &source : entry.at("sources")) {
      CHECK(source.at("identifier") != "testtooldomain");
    }
  }
}

TEST_CASE("supported games: a declared source with no provider gets no id",
          "[game_catalog]") {
  register_providers();
  engine::PluginLoader loader;
  fill_plugins(loader);
  const auto managed = make_managed();
  const auto doc     = build(loader, managed);

  const auto &entry = game(doc, "TestGameBeta");
  REQUIRE(entry.at("sources").size() == 1);
  const auto &only = entry.at("sources").front();
  // Reported, because the plugin declared it, but not invented an id for: a
  // wrong id would send a consumer to a source GMM cannot reach.
  CHECK(only.at("name") == "ModDB Direct");
  CHECK(only.at("source_id").is_null());
  CHECK(only.at("provenance") == "plugin");
}

TEST_CASE("supported games: the document matches the checked-in sample",
          "[game_catalog]") {
  register_providers();
  engine::PluginLoader loader;
  fill_plugins(loader);
  const auto managed = make_managed();
  const auto doc     = build(loader, managed);

  const fs::path sample =
      fs::path(__FILE__).parent_path() / "fixtures" / "supported_games_sample.json";
  std::ifstream in(sample);
  REQUIRE(in.is_open());
  nlohmann::json expected;
  in >> expected;

  // Parsed on both sides, so this pins the content rather than nlohmann's key
  // ordering; the ordering itself is pinned by the sorted-by-game_id contract.
  CHECK(doc == expected);
  REQUIRE(in.is_open());
}