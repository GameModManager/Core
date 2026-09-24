#pragma once

// v2 IPluginSaveParser backing store (MO2 ISaveParser parity). A plugin (v1 or
// v2 ABI) registers a save-file parser for a game via the register_save_parser
// callback; the engine stores it here keyed by game_id with a priority so the
// highest-priority parser for a game wins. The save scanner resolves the parser
// through parse_save() instead of hardcoding per-game parsers.
//
// Lifetime contract: a parser is a std::function that wraps the plugin's ABI
// callback (GmmSaveParserFn / GmmSaveParserFnV2) plus its user_data. The
// function object owns the captured ABI fn + user_data, so it stays valid only
// while the plugin is loaded. clear_plugin() drops every parser registered by a
// plugin's path before dlclose so no dangling ABI pointer survives.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine/game/saves/save_game.h"

namespace engine {

// Uniform parser signature after ABI wrapping: (save_path, game_id) -> SaveGame.
// Throws SaveParseError on malformed input (propagated to the scanner).
using SaveParserFn =
    std::function<SaveGame(const std::filesystem::path &, const std::string &)>;

// Fast-scan parser signature (Workspace-69xt). Same shape as SaveParserFn but
// under a CHEAP contract: header fields (pc_name, pc_level, pc_location,
// save_number, creation_time) plus the plugin lists (plugins, light_plugins,
// medium_plugins) ONLY. No screenshot (leave empty), no overlay/all_files
// (hover re-parses the full save on demand), no full-region decompression.
// The Saves scan worker prefers this when registered; the hover/detail paths
// keep using the full parser. Throws SaveParseError when the parser owns the
// format but rejects the file (scan skips it, MO2 listSaves parity).
using SaveFastParserFn =
    std::function<SaveGame(const std::filesystem::path &, const std::string &)>;

// One registered save parser. Mirrors the ABI registration tuple
// {game_id, fn, priority, user_data, plugin_path} exactly.
struct SaveParserEntry {
  std::string game_id;        // game this parser handles ("" = wildcard/all)
  SaveParserFn fn;            // wrapped ABI callback (captures user_data)
  int priority    = 0;        // higher wins on resolve
  void *user_data = nullptr;  // ABI user_data, captured by fn (kept for audit)
  std::string plugin_path;    // owning plugin path (for clear_plugin)
};

// Process-wide registry of save parsers. Thread-safe: registration happens on
// the pipeline thread during load; parse_save() is called from the saves scan
// worker thread.
class SaveParserRegistry {
public:
  static SaveParserRegistry &instance();

  // Register a parser for `game_id`. Higher priority wins on resolve.
  // `plugin_path` identifies the owning plugin so unload can drop it. A null
  // fn is ignored.
  void register_parser(std::string game_id, int priority, SaveParserFn fn,
                       void *user_data, std::string plugin_path);

  // Drop every parser registered by `plugin_path` (called from
  // PluginLoader::unload_all before dlclose so no dangling ABI pointer
  // survives). Drops full and fast parsers alike.
  void clear_plugin(const std::string &plugin_path);

  // Drop all parsers (process shutdown / full reload). Drops full and fast
  // parsers alike.
  void clear();

  // True if at least one parser is registered for `game_id`.
  [[nodiscard]] bool has_parser(const std::string &game_id) const;

  // Resolve + invoke the highest-priority parser for `game_id`. Returns
  // std::nullopt when no parser is registered; throws SaveParseError when the
  // parser rejects the file. The returned SaveGame is owned by the caller.
  [[nodiscard]] std::optional<SaveGame> parse_save(const std::filesystem::path &path,
                                                   const std::string &game_id) const;

  // Register a fast-scan parser for `game_id` (Workspace-69xt). Same
  // ownership/priority semantics as register_parser; honors the
  // SaveFastParserFn cheap contract above. A null fn is ignored.
  void register_fast_parser(std::string game_id, int priority, SaveFastParserFn fn,
                            void *user_data, std::string plugin_path);

  // True if at least one fast-scan parser is registered for `game_id`.
  [[nodiscard]] bool has_fast_parser(const std::string &game_id) const;

  // Resolve + invoke the highest-priority fast-scan parser for `game_id`.
  // Returns std::nullopt when none is registered; throws SaveParseError when
  // the parser rejects the file.
  [[nodiscard]] std::optional<SaveGame>
  parse_save_fast(const std::filesystem::path &path, const std::string &game_id) const;

private:
  SaveParserRegistry() = default;

  mutable std::mutex mutex_;
  std::vector<SaveParserEntry> entries_;
  std::vector<SaveParserEntry> fast_entries_;
};

}  // namespace engine
