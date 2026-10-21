#pragma once

// v3 Phase P0 - RegistrationSink: what registering a game plugin DOES.
//
// One sink per plugin load. Every registration effect (the writes the old
// cb_* handlers performed inline) routes through here, so the effect layer
// has exactly one addressable seam: the sink dispatches into the EXISTING
// registries (GameKnowledge, HookRegistry, SaveParserRegistry, ...) - it
// never owns a store of its own. The cb_* handlers keep the ABI marshaling
// (wrapping raw fn pointers into engine-side std::functions) and the log
// lines whose text differs between the v1 and v2 ABI paths; everything else
// routes through here.
//
// This header is ABI-free: it includes only engine headers that never pull
// gmm_abi_*.h transitively. Raw ABI callback pointers cross the seam as
// void* and are cast back at the registry call inside the .cpp (which gets
// the real types from the registry headers it includes).

#include "engine/game/registry/game_descriptor.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/plugin_host/save_parser_registry.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace engine {

class AnimationParserFeature;

class RegistrationSink {
public:
  RegistrationSink(PluginLoader &loader, PluginInfo &plugin)
      : loader_(&loader), plugin_(&plugin) {}

  // -- Game identity (v1 register_identity / v2 register_game) --
  //
  // Applies the descriptor onto the registering plugin's PluginInfo and
  // marks game support. dup_check_game_id is the identity the game-support
  // duplicate check keys on: the v2 adapter passes the ABI game_id ("" when
  // null), the v1 adapter passes the plugin's game_id (the .so stem - the
  // v1 ABI has no game_id argument). Only the first registrant of a given
  // key gets game_support = true.
  void add_entity(const GameDescriptor &desc, const std::string &dup_check_game_id);

  // -- Plugin metadata (v1 register_meta / v2 register_plugin) --
  // name is the v2 plugin name (null on the v1 path): when non-null it also
  // registers the plugin_name -> basename settings alias.
  void plugin_meta(const char *name, const char *author, const char *version,
                   const char *description);

  // -- Settings (v1 register_settings / v2 register_settings) --
  // name_alias is the v2 plugin_name (null on the v1 path).
  void settings_register(const char *const *keys, const char *const *values,
                         size_t count, const char *name_alias);
  void settings_tab_register(const char *title, const char *const *keys,
                             const char *const *types, const char *const *defaults,
                             const char *const *options, size_t count,
                             const char *name_alias);

  // -- v2 IPlugin requirements --
  void requirements_register(void *fn, void *user_data);

  // -- v2 IPluginDiagnose --
  void diagnostics_register(const char *game_id, void *fn, void *user_data);

  // -- Game features (v1 register_game_feature[_data] / v2 counterparts) --
  // game_id is null on the v2 path (scopes to the plugin's own game).
  void game_feature_register(const char *game_id, const char *feature_type,
                             int priority, const char *const *folder_names,
                             size_t folder_count, const char *const *file_extensions,
                             size_t extension_count);
  void game_feature_data_register(const char *game_id, const char *feature_type,
                                  int priority, const char *const *keys,
                                  const char *const *values, size_t count);

  // -- Hooks (v1 register_hook / v2 register_hook) --
  // knowledge_set stores the hook's data payload as game knowledge; the
  // handler calls it explicitly so the payload write stays a distinct,
  // individually addressable step.
  void knowledge_set(const std::string &game_id, const std::string &key,
                     const std::string &value);
  // Registers the hook function into the loader's engine::HookRegistry (the
  // fire() path). fn is the raw ABI hook pointer (GmmHookFn == GmmHookFnV2).
  void hook_register(const std::string &hook_tag, const std::string &hook_data,
                     void *fn, int priority, void *user_data);
  // v2-only: also registers the raw ABI hook into the process-wide v2
  // HookRegistry singleton (the dispatch() path).
  void hook_register_v2(const char *tag, void *fn, int priority, void *user_data,
                        const char *plugin_path);

  // -- v2 order encoding / deploy strategy --
  void order_encoding_register(void *fn, void *user_data);
  void deploy_strategy_register(void *deploy_fn, void *remove_fn, void *user_data);

  // -- v2 IPluginFileMapper --
  void file_mapper_register(const char *game_id, void *fn, void *user_data);

  // -- Sort provider (v1 register_sort_provider / v2 counterpart) --
  void sort_provider_register(const std::string &game_id, void *sort_fn,
                              void *user_data);

  // -- Stage claims (v1/v2 register_stage_claim + wildcard variants) --
  // game_id null/empty = the plugin's own game; empty game_id string =
  // wildcard (applies to all games).
  void stage_claim_register(const std::string &game_id, const std::string &stage_name,
                            StageFn handler, int priority);

  // -- v2 IPluginPreview --
  void preview_register(const char *file_extension, void *preview_data, void *fn,
                        void *user_data);

  // -- Tools (v1 register_tool / v2 register_tool) --
  // Builds the ExternalTool, registers it in the loader's ToolRegistry and,
  // when callback_registry is true (v2 only), also in the PluginToolRegistry.
  // Returns the built tool so the caller's log line can use it verbatim.
  ExternalTool tool_register(const char *tool_id, const char *kind, void *invoke_fn,
                             void *user_data, bool callback_registry);

  // -- v2 IPluginTool modpage download --
  void modpage_register(const char *url, void *fn, void *user_data);

  // -- Save parsers (v1/v2 register_save_parser) --
  // fn is the already-wrapped engine-side parser (the ABI marshaling lives
  // in the cb_ handler - it is ABI-version specific).
  void save_parser_register(const std::string &game_id, SaveParserFn fn, int priority,
                            void *user_data);

  // -- Animation parsers (v1/v2 register_animation_parser) --
  void animation_parser_register(const std::string &game_id,
                                 std::shared_ptr<AnimationParserFeature> feature,
                                 int priority, const std::string &source);

  // -- v2.1 tail-append slots --
  void game_validator_register(const char *game_id, void *fn, void *user_data);
  void game_variant_register(const char *game_id, const char *variant_id,
                             const char *display_name);
  void save_overlay_register(const char *game_id, void *fn, int priority,
                             void *user_data);

  // -- Categories (v1/v2 register_category / register_categories) --
  void category_register(const char *category);
  // Static on purpose: the original handler merges (and logs) even when the
  // bridge or current_plugin is null - the Category::Factory merge must run
  // regardless, keyed on the empty game_id in that case.
  static void categories_register(const PluginInfo *plugin, const int *ids,
                                  const char *const *names, const int *parent_ids,
                                  size_t count);

  // -- Capabilities / tabs (v1 register_capability + register_tab / v2
  //    register_tab) --
  void capability_register(const char *capability, const char *display_name,
                           const char *data_path, const char *description,
                           const char *protocol_handler, const char *website_domain,
                           const char *supported_platforms, const char *insert_before,
                           const char *insert_after);

private:
  PluginLoader *loader_ = nullptr;
  PluginInfo *plugin_   = nullptr;
};

}  // namespace engine
