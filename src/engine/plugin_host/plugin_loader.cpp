#include "engine/plugin_host/plugin_loader.h"
#include "engine/events/event_bus.h"
#include "engine/log/logger.h"
#include "engine/util/fs_utils.h"
#include "engine/vfs/path_resolver.h"
#include "engine/game/esp/plugin_database.h"
#include "engine/game/registry/game_features/game_feature.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/game/registry/game_registration_sink.h"
#include "engine/game/registry/capability_manifest.h"
#include "engine/game/saves/save_reader.h"
#include "engine/mod/meta/category_set_registry.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/fomod_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/plugin_host/category_factory.h"
#include "engine/plugin_host/deploy_strategy_registry.h"
#include "engine/plugin_host/diagnose_registry.h"
#include "engine/plugin_host/diagnostics_registry.h"
#include "engine/plugin_host/file_mapper_registry.h"
#include "engine/plugin_host/hook_registry.h"
#include "engine/plugin_host/order_encoding_registry.h"
#include "engine/plugin_host/plugin_settings_registry.h"
#include "engine/plugin_host/python_loader.h"
#include "engine/plugin_host/requirements_registry.h"
#include "engine/plugin_host/save_parser_registry.h"
#include "engine/plugin_host/tool_registry.h"
#include "engine/sort/sorter/abi.h"
#include "engine/sort/sorter/registry.h"

#include "gmm_abi_v1.h"
#include "gmm_abi_v2.h"
#include "gmm_abi_v3.h"

#include "platform/platform.h"

// v2 IPluginPreview backing store. Header-only + Qt-free so the engine can
// populate it without linking the UI library (keeps gmm_engine Qt-free and
// avoids an engine->ui link cycle). The UI casts the returned void* to
// QWidget*.
#include "ui/preview/preview_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

namespace engine {

// Bridging context passed as user_data to plugin registration callbacks
struct RegistrationBridge {
  PluginLoader *loader       = nullptr;
  PluginInfo *current_plugin = nullptr;
  // The per-load RegistrationSink every cb_* effect routes through. Set by
  // load_plugin right before the plugin's register function runs; the sink
  // wraps the same loader + current_plugin.
  RegistrationSink *sink = nullptr;
};

namespace {

  // PipelineContext of the stage claim currently executing on this thread. Set
  // around the plugin handler invocation so the host UI bridge callbacks
  // (GmmHostUi::fomod_wizard) can re-enter the engine on the same Mod + context
  // the plugin was handed. Only valid inside the handler; the host marshals any
  // UI onto the main thread itself, so the pointer stays valid for the whole
  // call, and the bridge refuses to run without it (e.g. called on a worker
  // thread spawned by the plugin).
  thread_local PipelineContext *g_active_stage_ctx = nullptr;

  // Minimal JSON string escaping for the host->plugin result payloads.
  std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned int>(c));
          out += buf;
        } else {
          out += c;
        }
      }
    }
    return out;
  }

  // Builds the engine-side stage handler for an ABI stage claim (v1 GmmStageFn
  // and v2 GmmStageFnV2 are the same pointer layout). Wraps the real engine
  // objects in the opaque handles the ABI promises and sets the thread-local
  // stage context so the host UI bridge can re-enter.
  StageFn make_stage_fn(void *fn) {
    return [fn](Mod &mod, PipelineContext &ctx_) -> bool {
      // Accessors in abi_bridge.cpp are null-safe, so a context without
      // instance/conflict/profile still works.
      GmmModHandle mod_h       = reinterpret_cast<GmmModHandle>(&mod);
      GmmInstanceHandle inst_h = reinterpret_cast<GmmInstanceHandle>(ctx_.instance);
      GmmConflictIndexHandle conf_h =
          reinterpret_cast<GmmConflictIndexHandle>(ctx_.conflict_index);
      GmmProfileHandle prof_h = reinterpret_cast<GmmProfileHandle>(ctx_.profile);
      // The context is live only for the plugin handler's call: the host UI
      // bridge (ctx.host_ui.*) re-enters engine stages on it, so the pointer
      // can never outlive the invoke.
      g_active_stage_ctx = &ctx_;
      const int result =
          reinterpret_cast<GmmStageFnV2>(fn)(mod_h, inst_h, conf_h, prof_h, nullptr);
      g_active_stage_ctx = nullptr;
      return result != 0;
    };
  }

}  // namespace

// ABI callback implementations
static void cb_register_identity(GmmRegistrationCtx *ctx, uint32_t steam_appid,
                                 const char *gog_id, const char *epic_namespace,
                                 const char *nexus_domain, const char *display_name,
                                 const char *exe_windows, const char *exe_linux,
                                 const char *exe_macos) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  GameDescriptor desc;
  desc.provenance  = DescriptorProvenance::CompiledV1;
  desc.plugin_path = bridge->current_plugin->path;
  desc.steam_appid = steam_appid;
  if (gog_id)
    desc.gog_id = gog_id;
  if (epic_namespace)
    desc.epic_namespace = epic_namespace;
  if (nexus_domain)
    desc.nexus_domain = nexus_domain;
  if (display_name)
    desc.display_name = display_name;
  if (exe_windows)
    desc.exe_windows = exe_windows;
  if (exe_linux)
    desc.exe_linux = exe_linux;
  if (exe_macos)
    desc.exe_macos = exe_macos;

  // The v1 ABI has no game_id argument: the duplicate check keys on the
  // plugin's game_id (the .so stem).
  bridge->sink->add_entity(desc, bridge->current_plugin->game_id);

  Logger::instance().debug(
      "Plugin registered identity: appid=" + std::to_string(steam_appid) +
      " name=" + (display_name ? display_name : bridge->current_plugin->game_id) +
      " nexus=" + (nexus_domain ? std::string(nexus_domain) : "none"));
}

static void cb_register_meta(GmmRegistrationCtx *ctx, const char *author,
                             const char *version, const char *description) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->plugin_meta(nullptr, author, version, description);
}

static void cb_register_category(GmmRegistrationCtx *ctx, const char *category) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->category_register(category);
}

static void cb_register_categories(GmmRegistrationCtx *ctx, const int *ids,
                                   const char *const *names, const int *parent_ids,
                                   size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  RegistrationSink::categories_register(
      bridge && bridge->current_plugin ? bridge->current_plugin : nullptr, ids, names,
      parent_ids, count);
}

static void cb_register_settings(GmmRegistrationCtx *ctx, const char *const *keys,
                                 const char *const *values, size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !keys || !values)
    return;

  bridge->sink->settings_register(keys, values, count, nullptr);
}

static void cb_register_settings_tab(GmmRegistrationCtx *ctx, const char *title,
                                     const char *const *keys, const char *const *types,
                                     const char *const *defaults,
                                     const char *const *options, size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !title || !keys || !types)
    return;

  bridge->sink->settings_tab_register(title, keys, types, defaults, options, count,
                                      nullptr);
}

static void cb_register_diagnostics(GmmRegistrationCtx *ctx, GmmDiagnosticsFn fn,
                                    void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  DiagnosticsRegistry::instance().register_provider(bridge->current_plugin->game_id, fn,
                                                    user_data);
}

static void cb_register_save_parser(GmmRegistrationCtx *ctx, const char *game_id,
                                    GmmSaveParserFn fn, int priority, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
  if (gid.empty() || !fn) {
    Logger::instance().warn("Save parser registered with empty game_id or null fn");
    return;
  }

  bridge->sink->save_parser_register(
      gid,
      [fn, user_data](const std::filesystem::path &path,
                      const std::string &game_id) -> SaveGame {
        GmmSaveGameC c_out = {};
        if (!fn(path.string().c_str(), game_id.c_str(), &c_out, user_data)) {
          throw SaveParseError("plugin parser returned 0");
        }
        SaveGame out;
        out.file_path     = c_out.file_path ? c_out.file_path : "";
        out.game_id       = c_out.game_id ? c_out.game_id : game_id;
        out.creation_time = c_out.creation_time;
        out.pc_name       = c_out.pc_name ? c_out.pc_name : "";
        out.pc_level      = c_out.pc_level;
        out.pc_location   = c_out.pc_location ? c_out.pc_location : "";
        out.save_number   = c_out.save_number;
        for (uint32_t i = 0; i < c_out.plugin_count && i < GMM_SAVE_MAX_PLUGINS; ++i) {
          out.plugins.push_back(c_out.plugins[i] ? c_out.plugins[i] : "");
          free(c_out.plugins[i]);
        }
        for (uint32_t i = 0; i < c_out.light_plugin_count && i < GMM_SAVE_MAX_PLUGINS;
             ++i) {
          out.light_plugins.push_back(c_out.light_plugins[i] ? c_out.light_plugins[i]
                                                             : "");
          free(c_out.light_plugins[i]);
        }
        free(c_out.file_path);
        free(c_out.game_id);
        free(c_out.pc_name);
        free(c_out.pc_location);
        return out;
      },
      priority, user_data);
  Logger::instance().debug("Plugin registered save parser for game=" + gid);
}

static void cb_register_animation_parser(GmmRegistrationCtx *ctx, const char *game_id,
                                         const char *file_extension,
                                         GmmAnimationParserFn fn, int priority,
                                         void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  // A NULL game_id means the parser is non-game-specific (file-format based,
  // applies to every game) and is registered under the empty/global game_id.
  // This is what lets a generic plugin such as ANM2 serve any game
  // without being re-registered per game. An explicit game_id scopes the
  // parser to that one game (and overrides any global parser for it).
  std::string gid = game_id ? game_id : "";
  if (!fn) {
    Logger::instance().warn("Animation parser registered with null fn");
    return;
  }

  auto feature = std::make_shared<AnimationParserFeature>(
      [fn, user_data](const std::string &file_path, const std::string &base_dir)
          -> std::optional<AnimationParserFeature::AnimationData> {
        GmmAnimationDataC c_out = {};
        if (!fn(file_path.c_str(), base_dir.c_str(), &c_out, user_data)) {
          return std::nullopt;
        }
        AnimationParserFeature::AnimationData data;
        data.fps           = c_out.fps;
        data.canvas_width  = c_out.canvas_width;
        data.canvas_height = c_out.canvas_height;
        for (size_t fi = 0; fi < c_out.frame_count; ++fi) {
          auto &cf = c_out.frames[fi];
          AnimationParserFeature::Frame frame;
          frame.delay_ms = cf.delay_ms;
          for (size_t li = 0; li < cf.layer_count; ++li) {
            auto &cl = cf.layers[li];
            AnimationParserFeature::LayerItem layer;
            layer.x      = cl.x;
            layer.y      = cl.y;
            layer.width  = cl.width;
            layer.height = cl.height;
            if (cl.rgba_pixels && cl.pixel_count > 0) {
              layer.rgba_pixels.assign(cl.rgba_pixels, cl.rgba_pixels + cl.pixel_count);
              free(cl.rgba_pixels);
            }
            frame.layers.push_back(std::move(layer));
          }
          data.frames.push_back(std::move(frame));
          free(cf.layers);
        }
        free(c_out.frames);
        return data;
      });

  bridge->sink->animation_parser_register(gid, std::move(feature), priority,
                                          bridge->current_plugin->path);
  Logger::instance().debug("Plugin registered animation parser for game=" + gid);
}

static void cb_register_game_feature(GmmRegistrationCtx *ctx, const char *game_id,
                                     const char *feature_type, int priority,
                                     const char *const *folder_names,
                                     size_t folder_count,
                                     const char *const *file_extensions,
                                     size_t extension_count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->game_feature_register(game_id, feature_type, priority, folder_names,
                                      folder_count, file_extensions, extension_count);
}

static void cb_register_game_feature_data(GmmRegistrationCtx *ctx, const char *game_id,
                                          const char *feature_type, int priority,
                                          const char *const *keys,
                                          const char *const *values, size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->game_feature_data_register(game_id, feature_type, priority, keys,
                                           values, count);
}

static void cb_subscribe_event(GmmRegistrationCtx *ctx, const char *event_id,
                               GmmEventFn fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;
  if (!event_id || !fn) {
    Logger::instance().warn("subscribe_event called with null event_id/fn");
    return;
  }

  // The bus owns the std::function, so the captured GmmEventFn+user_data
  // live for the subscription's lifetime; unload_all() drops every
  // subscription registered under this plugin's path before dlclose, so the
  // pointer never outlives the .so that owns fn.
  engine::EventBus::instance().subscribe(
      event_id,
      [fn, user_data](const std::string &eid, const std::string &payload) {
        fn(eid.c_str(), payload.c_str(), user_data);
      },
      bridge->current_plugin->path);

  Logger::instance().debug("Plugin subscribed to event: " + std::string(event_id) +
                           " (plugin=" + bridge->current_plugin->game_id + ")");
}

// P1.4 - GmmHostUi::fomod_wizard: the plugin's Fomod stage handler asks the
// host to run the FOMOD wizard + install for the mod it is processing. The
// engine's Qt-free FomodStage does all the work (detect fomod/, parse
// ModuleConfig.xml, apply the chosen options to the staging dir, flatten);
// the wizard itself is the host's fomod_query_cb (wired by the UI to the
// native dialog), invoked on the pipeline thread exactly as in the core
// FomodStage path. The outcome goes back to the plugin as JSON.
//
// No ctx is passed: the pipeline context of the plugin's running stage comes
// from the thread-local set around the handler invocation, so the plugin can
// call this from a handler it cached the function pointer of - never from a
// cached GmmRegistrationCtx (that is host storage, valid only for
// gmm_register_v1).
static int cb_fomod_wizard(GmmModHandle mod, char *out_json, size_t out_capacity) {
  auto *m = reinterpret_cast<Mod *>(mod);
  if (!m || !out_json || out_capacity == 0)
    return 0;
  out_json[0] = '\0';

  PipelineContext *pctx = g_active_stage_ctx;
  if (!pctx) {
    Logger::instance().warn("host_ui.fomod_wizard called outside a stage handler");
    return 0;
  }

  FomodStage stage;
  const bool ok = stage.execute(*m, *pctx);

  // Serialize the outcome. fomod_detected separates "not a FOMOD"
  // (pass-through) from a real FOMOD install; canceled vs failed via the
  // stage's own context flag (FomodStage sets ctx.canceled on wizard
  // cancel). choices carries the persisted fomod.json object verbatim.
  std::string json;
  if (!pctx->fomod_detected) {
    json = "{\"outcome\":\"not_fomod\"}";
  } else if (pctx->canceled) {
    json = "{\"outcome\":\"canceled\"}";
  } else if (!ok) {
    json = "{\"outcome\":\"failed\"}";
  } else {
    json = "{\"outcome\":\"installed\",\"final_name\":\"" + json_escape(m->name) +
           "\",\"choices\":" +
           (pctx->fomod_choices_json.empty() ? "null" : pctx->fomod_choices_json) + "}";
  }
  if (json.size() >= out_capacity) {
    Logger::instance().warn(
        "host_ui.fomod_wizard: result does not fit the plugin buffer");
    return 0;
  }
  std::memcpy(out_json, json.c_str(), json.size() + 1);
  return 1;
}

/* v2 wrapper: casts void* back to GmmModHandle for the v1 cb_fomod_wizard */
static int cb_fomod_wizard_v2(void *mod, char *out_json, size_t out_capacity) {
  return cb_fomod_wizard(static_cast<GmmModHandle>(mod), out_json, out_capacity);
}

static void cb_register_stage_claim(GmmRegistrationCtx *ctx, const char *stage_name,
                                    GmmStageFn fn, int priority) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string stage = stage_name ? stage_name : "";
  if (stage.empty() || !fn)
    return;

  bridge->sink->stage_claim_register(bridge->current_plugin->game_id, stage,
                                     make_stage_fn(reinterpret_cast<void *>(fn)),
                                     priority);
}

static void cb_register_wildcard_stage_claim(GmmRegistrationCtx *ctx,
                                             const char *game_id,
                                             const char *stage_name, GmmStageFn fn,
                                             int priority) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  // NULL/empty game_id = wildcard (applies to all games)
  std::string gid   = game_id ? game_id : "";
  std::string stage = stage_name ? stage_name : "";
  if (stage.empty() || !fn)
    return;

  bridge->sink->stage_claim_register(
      gid, stage, make_stage_fn(reinterpret_cast<void *>(fn)), priority);
}

static void cb_register_hook(GmmRegistrationCtx *ctx, const char *tag, const char *data,
                             GmmHookFn fn, int priority, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string game_id   = bridge->current_plugin->game_id;
  std::string hook_tag  = tag ? tag : "";
  std::string hook_data = data ? data : "";

  // Store as game knowledge - key=tag, value=data
  bridge->sink->knowledge_set(game_id, hook_tag, hook_data);

  // Register the hook function into HookRegistry so the engine can fire
  // it at pipeline points (before_deploy, after_scan, conflict_resolution).
  // Note: GmmHookFn expects void* data, so we const_cast the string data.
  bridge->sink->hook_register(hook_tag, hook_data, reinterpret_cast<void *>(fn),
                              priority, user_data);
}

static void cb_register_order_encoding(GmmRegistrationCtx *ctx, GmmOrderEncodingFn fn) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge)
    return;

  // TODO: Store order encoding callback in plugin info for pipeline use
  Logger::instance().debug("Plugin registered order encoding hook");
  (void)fn;
}

static void cb_register_deploy_strategy(GmmRegistrationCtx *ctx, GmmDeployFn deploy_fn,
                                        GmmRemoveFn remove_fn) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge)
    return;

  Logger::instance().debug("Plugin registered deploy strategy");
  (void)deploy_fn;
  (void)remove_fn;
}

// Convert snake_case tool id to Title Case for display (e.g. "event_bus_viewer"
// -> "Event Bus Viewer"). Lives in game_registration_sink.cpp; the sink owns
// the tool registration effect.

static void cb_register_tool(GmmRegistrationCtx *ctx, const char *tool_id,
                             const char *kind, void (*invoke_fn)(void *),
                             void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string kind_str    = kind ? kind : "advisory";
  const ExternalTool tool = bridge->sink->tool_register(
      tool_id, kind, reinterpret_cast<void *>(invoke_fn), user_data, false);

  Logger::instance().debug("Plugin registered tool: " + tool.tool_id + " (" + kind_str +
                           ") for game=" + tool.game_id);
}

static void cb_register_image_diff(GmmRegistrationCtx *ctx, GmmImageDiffFn fn,
                                   void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge)
    return;

  bridge->loader->register_image_diff(fn, user_data);

  Logger::instance().debug("Plugin registered image diff provider");
}

static void cb_register_sort_provider(GmmRegistrationCtx *ctx, const char *game_id,
                                      SortFn sort_fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge)
    return;

  std::string gid = game_id ? game_id : "";
  if (gid.empty()) {
    Logger::instance().warn("Sort provider registered with empty game_id");
    return;
  }

  bridge->sink->sort_provider_register(gid, reinterpret_cast<void *>(sort_fn),
                                       user_data);

  Logger::instance().debug("Plugin registered sort provider for game=" + gid);
}

static void cb_register_capability(GmmRegistrationCtx *ctx, const char *capability,
                                   const char *display_name, const char *data_path,
                                   const char *description,
                                   const char *protocol_handler,
                                   const char *website_domain,
                                   const char *supported_platforms) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->capability_register(capability, display_name, data_path, description,
                                    protocol_handler, website_domain,
                                    supported_platforms, nullptr, nullptr);
}

static void cb_register_tab(GmmRegistrationCtx *ctx, const char *capability,
                            const char *display_name, const char *data_path,
                            const char *description, const char *protocol_handler,
                            const char *website_domain, const char *supported_platforms,
                            const char *insert_before, const char *insert_after) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->capability_register(capability, display_name, data_path, description,
                                    protocol_handler, website_domain,
                                    supported_platforms, insert_before, insert_after);
}

// ---------------------------------------------------------------------------
// v2 ABI callbacks
//
// These mirror the v1 RegistrationBridge callbacks but operate on the v2
// GmmRegistrationCtxV2 and store into the same internal registries (or, for
// v2-only concepts with no v1 equivalent, onto PluginInfo). The v1 path is
// left untouched; the two sets of callbacks share only the RegistrationBridge
// plumbing (loader + current_plugin).
// ---------------------------------------------------------------------------

static void cb_v2_register_plugin(GmmRegistrationCtxV2 *ctx, GmmPluginInfo info) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->plugin_meta(info.name, info.author, info.version, info.description);
}

static void cb_v2_register_settings(GmmRegistrationCtxV2 *ctx, const char *const *keys,
                                    const char *const *values, size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !keys || !values)
    return;

  bridge->sink->settings_register(keys, values, count,
                                  bridge->current_plugin->plugin_name.c_str());
}

static void cb_v2_register_settings_tab(GmmRegistrationCtxV2 *ctx, const char *title,
                                        const char *const *keys,
                                        const char *const *types,
                                        const char *const *defaults,
                                        const char *const *options, size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !title || !keys || !types)
    return;

  bridge->sink->settings_tab_register(title, keys, types, defaults, options, count,
                                      bridge->current_plugin->plugin_name.c_str());
}

static void cb_v2_register_requirements(GmmRegistrationCtxV2 *ctx, GmmRequirementsFn fn,
                                        void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !fn)
    return;

  bridge->sink->requirements_register(reinterpret_cast<void *>(fn), user_data);
}

static void cb_v2_register_diagnostics(GmmRegistrationCtxV2 *ctx, const char *game_id,
                                       GmmDiagnoseFn fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !fn)
    return;

  bridge->sink->diagnostics_register(game_id, reinterpret_cast<void *>(fn), user_data);
}

static void cb_v2_register_game(GmmRegistrationCtxV2 *ctx, GmmGameInfo info) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  GameDescriptor desc;
  desc.provenance  = DescriptorProvenance::CompiledV2;
  desc.plugin_path = bridge->current_plugin->path;
  if (info.game_id)
    desc.id = info.game_id;
  if (info.display_name)
    desc.display_name = info.display_name;
  desc.steam_appid = info.steam_appid;
  if (info.nexus_domain)
    desc.nexus_domain = info.nexus_domain;
  if (info.gog_id)
    desc.gog_id = info.gog_id;
  if (info.epic_namespace)
    desc.epic_namespace = info.epic_namespace;
  if (info.exe_windows)
    desc.exe_windows = info.exe_windows;
  if (info.exe_linux)
    desc.exe_linux = info.exe_linux;
  if (info.exe_macos)
    desc.exe_macos = info.exe_macos;

  bridge->sink->add_entity(desc, info.game_id ? info.game_id : "");

  Logger::instance().debug(
      "Plugin registered game: id=" + bridge->current_plugin->game_id +
      " name=" + bridge->current_plugin->game_display_name +
      " nexus=" + (info.nexus_domain ? std::string(info.nexus_domain) : "none"));
}

static void cb_v2_register_game_feature(GmmRegistrationCtxV2 *ctx,
                                        const char *feature_type, int priority,
                                        const char *const *folder_names,
                                        size_t folder_count,
                                        const char *const *file_extensions,
                                        size_t extension_count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  // v2 has no explicit game_id on this call; scope to the plugin's own game.
  bridge->sink->game_feature_register(nullptr, feature_type, priority, folder_names,
                                      folder_count, file_extensions, extension_count);
}

static void cb_v2_register_game_feature_data(GmmRegistrationCtxV2 *ctx,
                                             const char *feature_type, int priority,
                                             const char *const *keys,
                                             const char *const *values, size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->game_feature_data_register(nullptr, feature_type, priority, keys,
                                           values, count);
}

static void cb_v2_register_hook(GmmRegistrationCtxV2 *ctx, const char *tag,
                                const char *data, GmmHookFnV2 fn, int priority,
                                void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string game_id   = bridge->current_plugin->game_id;
  std::string hook_tag  = tag ? tag : "";
  std::string hook_data = data ? data : "";

  // Store as game knowledge (data payload) - the v1 path's behaviour.
  bridge->sink->knowledge_set(game_id, hook_tag, hook_data);

  // Register the hook function into the instance-based engine::HookRegistry
  // (v1-style) so the engine can fire it at pipeline points
  // (before_deploy, after_scan, conflict_resolution) through the existing
  // fire() path. The hook fires with the tag + data + user_data the plugin
  // provided.
  bridge->sink->hook_register(hook_tag, hook_data, reinterpret_cast<void *>(fn),
                              priority, user_data);

  // Also register the raw v2 ABI hook into the v2 HookRegistry singleton so
  // the pipeline can dispatch it directly via
  // HookRegistry::instance().dispatch() with an arbitrary data pointer (the v2
  // GmmHookFnV2 contract).
  bridge->sink->hook_register_v2(tag, reinterpret_cast<void *>(fn), priority, user_data,
                                 bridge->current_plugin->path.c_str());
}

static void cb_v2_register_order_encoding(GmmRegistrationCtxV2 *ctx,
                                          GmmOrderEncodingFnV2 fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->order_encoding_register(reinterpret_cast<void *>(fn), user_data);
}

static void cb_v2_register_deploy_strategy(GmmRegistrationCtxV2 *ctx,
                                           GmmDeployFnV2 deploy_fn,
                                           GmmRemoveFnV2 remove_fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->deploy_strategy_register(reinterpret_cast<void *>(deploy_fn),
                                         reinterpret_cast<void *>(remove_fn),
                                         user_data);
}

static void cb_v2_register_file_mapper(GmmRegistrationCtxV2 *ctx, const char *game_id,
                                       GmmFileMapperFn fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !fn)
    return;

  bridge->sink->file_mapper_register(game_id, reinterpret_cast<void *>(fn), user_data);
}

static void cb_v2_register_sort_provider(GmmRegistrationCtxV2 *ctx, GmmSortFn sort_fn,
                                         void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge)
    return;

  std::string gid = bridge->current_plugin ? bridge->current_plugin->game_id : "";
  if (gid.empty()) {
    Logger::instance().warn("Sort provider registered with empty game_id");
    return;
  }

  bridge->sink->sort_provider_register(gid, reinterpret_cast<void *>(sort_fn),
                                       user_data);

  Logger::instance().debug("Plugin registered v2 sort provider for game=" + gid);
}

static void cb_v2_register_stage_claim(GmmRegistrationCtxV2 *ctx,
                                       const char *stage_name, GmmStageFnV2 fn,
                                       int priority) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string stage = stage_name ? stage_name : "";
  if (stage.empty() || !fn)
    return;

  bridge->sink->stage_claim_register(bridge->current_plugin->game_id, stage,
                                     make_stage_fn(reinterpret_cast<void *>(fn)),
                                     priority);
}

static void cb_v2_register_wildcard_stage_claim(GmmRegistrationCtxV2 *ctx,
                                                const char *game_id,
                                                const char *stage_name, GmmStageFnV2 fn,
                                                int priority) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string gid   = game_id ? game_id : "";
  std::string stage = stage_name ? stage_name : "";
  if (stage.empty() || !fn)
    return;

  bridge->sink->stage_claim_register(
      gid, stage, make_stage_fn(reinterpret_cast<void *>(fn)), priority);
}

static char *cb_v2_resolve_file(const char *root, const char *relative_path,
                                void *user_data) {
  if (!root || !relative_path)
    return nullptr;
  const auto gf = engine::vfs::PathResolver(std::filesystem::path(root))
                      .resolve(std::string(relative_path));
  if (!gf)
    return nullptr;
  const std::string s = gf->absolute().string();
  char *out           = static_cast<char *>(std::malloc(s.size() + 1));
  if (out) {
    std::memcpy(out, s.c_str(), s.size() + 1);
  }
  return out;
}

static void cb_v2_register_preview(GmmRegistrationCtxV2 *ctx,
                                   const char *file_extension, void *preview_data,
                                   GmmPreviewFn fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  Logger::instance().debug(
      "[PluginLoader] cb_v2_register_preview: ctx=" +
      std::to_string(reinterpret_cast<uintptr_t>(ctx)) +
      " file_extension=" + (file_extension ? std::string(file_extension) : "NULL") +
      " fn=" + std::to_string(reinterpret_cast<uintptr_t>(fn)) +
      " user_data=" + std::to_string(reinterpret_cast<uintptr_t>(user_data)));
  if (!bridge || !bridge->current_plugin || !fn) {
    Logger::instance().debug(
        "[PluginLoader] cb_v2_register_preview: ABORT bridge=" +
        std::to_string(reinterpret_cast<uintptr_t>(bridge)) +
        " plugin=" + (bridge && bridge->current_plugin ? "valid" : "NULL"));
    return;
  }
  Logger::instance().debug("[PluginLoader] cb_v2_register_preview: plugin=" +
                           bridge->current_plugin->plugin_name +
                           " path=" + bridge->current_plugin->path);

  bridge->sink->preview_register(file_extension, preview_data,
                                 reinterpret_cast<void *>(fn), user_data);
}

static void cb_v2_register_tool(GmmRegistrationCtxV2 *ctx, const char *tool_id,
                                const char *kind, GmmToolInvokeFn fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string kind_str    = kind ? kind : "advisory";
  const ExternalTool tool = bridge->sink->tool_register(
      tool_id, kind, reinterpret_cast<void *>(fn), user_data, true);

  Logger::instance().debug("Plugin registered v2 tool: " + tool.tool_id + " (" +
                           kind_str + ") for game=" + tool.game_id);
}

static void cb_v2_register_modpage(GmmRegistrationCtxV2 *ctx, const char *url,
                                   GmmModPageDownloadFn fn, void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin || !fn)
    return;

  bridge->sink->modpage_register(url, reinterpret_cast<void *>(fn), user_data);
}

static void cb_v2_register_save_parser(GmmRegistrationCtxV2 *ctx, const char *game_id,
                                       GmmSaveParserFnV2 fn, int priority,
                                       void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  std::string gid = game_id ? game_id : bridge->current_plugin->game_id;
  if (gid.empty() || !fn) {
    Logger::instance().warn("Save parser registered with empty game_id or null fn");
    return;
  }

  // Snapshot the registering plugin's v2.1 feature bits so the bridge
  // copies new GmmSaveDataV2 fields ONLY when the plugin proves support
  // via gmm_abi_features. Plugins built before v2.1 leave this at 0 and
  // get the pre-v2.1 copy path (no new fields read). Load-time only, so
  // no concurrency concern.
  const uint64_t features   = bridge->current_plugin->features;
  const bool has_screenshot = (features & GMM_FEATURE_SAVE_SCREENSHOT) != 0;
  const bool has_medium     = (features & GMM_FEATURE_SAVE_MEDIUM) != 0;
  const bool has_all_files  = (features & GMM_FEATURE_SAVE_ALL_FILES) != 0;
  // Keep a reference to the loader so the wrapper can resolve the
  // overlay fn at parse time (register_save_overlay may fire AFTER
  // register_save_parser inside the plugin, so we cannot capture the
  // overlay fn at registration time). Loader pointer outlives the
  // lambda: load_directory finishes before any save scan runs.
  PluginLoader *loader = bridge->loader;

  bridge->sink->save_parser_register(
      gid,
      [fn, user_data, has_screenshot, has_medium, has_all_files, gid, loader](
          const std::filesystem::path &path, const std::string &game_id) -> SaveGame {
        GmmSaveDataV2 c_out = {};
        if (!fn(path.string().c_str(), game_id.c_str(), &c_out, user_data)) {
          throw SaveParseError("plugin v2 parser returned 0");
        }
        SaveGame out;
        out.file_path     = c_out.file_path ? c_out.file_path : "";
        out.game_id       = c_out.game_id ? c_out.game_id : game_id;
        out.creation_time = c_out.creation_time;
        out.pc_name       = c_out.pc_name ? c_out.pc_name : "";
        out.pc_level      = c_out.pc_level;
        out.pc_location   = c_out.pc_location ? c_out.pc_location : "";
        out.save_number   = c_out.save_number;
        for (uint32_t i = 0; i < c_out.plugin_count && i < 256; ++i) {
          out.plugins.push_back(c_out.plugins[i] ? c_out.plugins[i] : "");
          free(c_out.plugins[i]);
        }
        for (uint32_t i = 0; i < c_out.light_plugin_count && i < 256; ++i) {
          out.light_plugins.push_back(c_out.light_plugins[i] ? c_out.light_plugins[i]
                                                             : "");
          free(c_out.light_plugins[i]);
        }

        // v2.1+ tail-append fields: gate on the plugin's feature bits.
        // Pre-v2.1 plugins never set these bits, so the engine reads
        // them as zeroed struct members (memset at function start) and
        // leaves the SaveGame fields empty - exactly the v2.0 behavior.
        if (has_screenshot && c_out.screenshot_rgba && c_out.screenshot_size &&
            c_out.screenshot_width > 0 && c_out.screenshot_height > 0) {
          const size_t expected =
              static_cast<size_t>(c_out.screenshot_width) * c_out.screenshot_height * 4;
          if (c_out.screenshot_size >= expected) {
            out.screenshot.assign(c_out.screenshot_rgba,
                                  c_out.screenshot_rgba + expected);
            out.screenshot_width  = c_out.screenshot_width;
            out.screenshot_height = c_out.screenshot_height;
          }
        }
        if (has_medium) {
          for (uint32_t i = 0;
               i < c_out.medium_plugin_count && i < GMM_SAVE_MAX_PLUGINS; ++i) {
            out.medium_plugins.push_back(
                c_out.medium_plugins[i] ? c_out.medium_plugins[i] : "");
            free(c_out.medium_plugins[i]);
          }
        }
        if (has_all_files && c_out.all_files) {
          for (uint32_t i = 0; i < c_out.all_files_count; ++i) {
            out.all_files.push_back(c_out.all_files[i] ? c_out.all_files[i] : "");
            free(c_out.all_files[i]);
          }
          // The outer char** array itself is plugin-malloc'd per the ABI
          // ownership comment; free it now that we've copied all strings.
          free(c_out.all_files);
        }

        // Save overlay: if some plugin registered an overlay fn for this
        // game, invoke it on the freshly-parsed GmmSaveDataV2 and copy
        // the kv rows into SaveGame::overlay. The overlay builder owns
        // the malloc'd GmmSaveOverlayV2 + its strings; we free them all
        // before returning. Mirrors the pre-v2.1 field ownership.
        if (loader) {
          for (const auto &p : loader->plugins()) {
            if (p.save_overlay_fn && p.game_id == gid) {
              auto overlay_fn = reinterpret_cast<GmmSaveOverlayFnV2>(p.save_overlay_fn);
              if (GmmSaveOverlayV2 *ov = overlay_fn(&c_out, p.save_overlay_user_data)) {
                if (ov->kv_keys && ov->kv_values) {
                  for (uint32_t i = 0; i < ov->kv_count; ++i) {
                    out.overlay.push_back({ov->kv_keys[i] ? ov->kv_keys[i] : "",
                                           ov->kv_values[i] ? ov->kv_values[i] : ""});
                    free(ov->kv_keys[i]);
                    free(ov->kv_values[i]);
                  }
                }
                free(ov->kv_keys);
                free(ov->kv_values);
                free(ov->title);
                free(ov->subtitle);
                free(ov);
              }
              break;  // first overlay wins (single-registration contract)
            }
          }
        }

        // Free the pre-v2.1 string fields.
        free(c_out.file_path);
        free(c_out.game_id);
        free(c_out.pc_name);
        free(c_out.pc_location);
        // Free v2.1+ plugin-malloc'd buffers (screenshot + medium strings
        // were either copied above and we must still free the plugin's
        // copy, OR not copied because the feature bit was off - in which
        // case they're zeroed by the struct-init above and free(NULL)
        // is well-defined).
        free(c_out.screenshot_rgba);
        return out;
      },
      priority, user_data);
  Logger::instance().debug("Plugin registered v2 save parser for game=" + gid);
}

// v2.1+ additive tail-append slots. See gmm_abi_v2.h 3a1484c + Workspace-vc69.
// Each callback mirrors the MO2 IPluginGame / IPluginSaveParser registration
// shape (no Qt types across the ABI, plain data + function pointers only).
// The engine stores the captured data on PluginInfo; consumers (game
// detection, instance/game model, Saves tab) reach in there. We deliberately
// do NOT invoke these callbacks at registration time beyond capturing them -
// they're cheap to keep and the rest of the engine consults them lazily.
static void cb_v2_register_game_validator(GmmRegistrationCtxV2 *ctx,
                                          const char *game_id, GmmLooksValidFn fn,
                                          void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->game_validator_register(game_id, reinterpret_cast<void *>(fn),
                                        user_data);
}

static void cb_v2_register_game_variant(GmmRegistrationCtxV2 *ctx, const char *game_id,
                                        const char *variant_id,
                                        const char *display_name) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->game_variant_register(game_id, variant_id, display_name);
}

static void cb_v2_register_save_overlay(GmmRegistrationCtxV2 *ctx, const char *game_id,
                                        GmmSaveOverlayFnV2 fn, int priority,
                                        void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->save_overlay_register(game_id, reinterpret_cast<void *>(fn), priority,
                                      user_data);
}

static void cb_v2_register_animation_parser(GmmRegistrationCtxV2 *ctx,
                                            const char *game_id,
                                            const char *file_extension,
                                            GmmAnimationParserFnV2 fn, int priority,
                                            void *user_data) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  // A NULL game_id means the parser is non-game-specific (file-format based,
  // applies to every game) and is registered under the empty/global game_id.
  std::string gid = game_id ? game_id : "";
  if (!fn) {
    Logger::instance().warn("Animation parser registered with null fn");
    return;
  }

  auto feature = std::make_shared<AnimationParserFeature>(
      [fn, user_data](const std::string &file_path, const std::string &base_dir)
          -> std::optional<AnimationParserFeature::AnimationData> {
        GmmAnimationDataV2 c_out = {};
        if (!fn(file_path.c_str(), base_dir.c_str(), &c_out, user_data)) {
          return std::nullopt;
        }
        AnimationParserFeature::AnimationData data;
        data.fps           = c_out.fps;
        data.canvas_width  = c_out.canvas_width;
        data.canvas_height = c_out.canvas_height;

        // Pass through the raw animation pointer from the plugin for
        // on-demand rendering. The plugin owns this memory.
        data.raw_animation           = c_out.raw_animation;
        data.on_demand_canvas_width  = c_out.canvas_width;
        data.on_demand_canvas_height = c_out.canvas_height;
        data.on_demand_fps           = static_cast<int>(c_out.fps);
        data.on_demand_frame_count   = static_cast<int>(c_out.frame_count);

        // Wrap the plugin's render callback for on-demand frame generation.
        if (c_out.render_frame && c_out.raw_animation) {
          auto *raw       = c_out.raw_animation;
          auto *render_fn = c_out.render_frame;
          data.render_frame =
              [raw, render_fn](float time_ms) -> AnimationParserFeature::RenderResult {
            AnimationParserFeature::RenderResult result;
            int32_t w = 0, h = 0;
            uint8_t *pixels = render_fn(raw, time_ms, &w, &h);
            if (pixels && w > 0 && h > 0) {
              result.pixels.assign(pixels, pixels + (static_cast<size_t>(w) *
                                                     static_cast<size_t>(h) * 4));
              result.width  = w;
              result.height = h;
              free(pixels);
            }
            return result;
          };
        }

        for (size_t fi = 0; fi < c_out.frame_count; ++fi) {
          auto &cf = c_out.frames[fi];
          AnimationParserFeature::Frame frame;
          frame.delay_ms = cf.delay_ms;
          for (size_t li = 0; li < cf.layer_count; ++li) {
            auto &cl = cf.layers[li];
            AnimationParserFeature::LayerItem layer;
            layer.x      = cl.x;
            layer.y      = cl.y;
            layer.width  = cl.width;
            layer.height = cl.height;
            if (cl.rgba_pixels && cl.pixel_count > 0) {
              layer.rgba_pixels.assign(cl.rgba_pixels, cl.rgba_pixels + cl.pixel_count);
              free(cl.rgba_pixels);
            }
            frame.layers.push_back(std::move(layer));
          }
          data.frames.push_back(std::move(frame));
          free(cf.layers);
        }
        free(c_out.frames);

        /* Convert named animation states (new in ABI v2.1) */
        for (size_t si = 0; si < c_out.state_count; ++si) {
          auto &cs = c_out.states[si];
          AnimationParserFeature::AnimationState state;
          if (cs.name) {
            state.name = std::string(cs.name);
            free(cs.name);
          }
          state.canvas_width  = cs.canvas_width;
          state.canvas_height = cs.canvas_height;
          state.raw_animation = cs.raw_animation;
          // Create per-state render callback that captures this state's own
          // raw_animation pointer (not the top-level one which is the first state).
          if (cs.render_frame && cs.raw_animation) {
            auto *raw       = cs.raw_animation;
            auto *render_fn = cs.render_frame;
            state.render_frame =
                [raw,
                 render_fn](float time_ms) -> AnimationParserFeature::RenderResult {
              AnimationParserFeature::RenderResult result;
              int32_t w = 0, h = 0;
              uint8_t *pixels = render_fn(raw, time_ms, &w, &h);
              if (pixels && w > 0 && h > 0) {
                result.pixels.assign(pixels, pixels + (static_cast<size_t>(w) *
                                                       static_cast<size_t>(h) * 4));
                result.width  = w;
                result.height = h;
                free(pixels);
              }
              return result;
            };
            state.on_demand_canvas_width  = cs.canvas_width;
            state.on_demand_canvas_height = cs.canvas_height;
            state.on_demand_fps           = static_cast<int>(c_out.fps);
            state.on_demand_frame_count   = static_cast<int>(cs.frame_count);
          }
          for (size_t fi = 0; fi < cs.frame_count; ++fi) {
            auto &cf = cs.frames[fi];
            AnimationParserFeature::Frame frame;
            frame.delay_ms = cf.delay_ms;
            for (size_t li = 0; li < cf.layer_count; ++li) {
              auto &cl = cf.layers[li];
              AnimationParserFeature::LayerItem layer;
              layer.x      = cl.x;
              layer.y      = cl.y;
              layer.width  = cl.width;
              layer.height = cl.height;
              if (cl.rgba_pixels && cl.pixel_count > 0) {
                layer.rgba_pixels.assign(cl.rgba_pixels,
                                         cl.rgba_pixels + cl.pixel_count);
                free(cl.rgba_pixels);
              }
              frame.layers.push_back(std::move(layer));
            }
            state.frames.push_back(std::move(frame));
            free(cf.layers);
          }
          free(cs.frames);
          data.states.push_back(std::move(state));
        }
        free(c_out.states);

        return data;
      });

  bridge->sink->animation_parser_register(gid, std::move(feature), priority,
                                          bridge->current_plugin->path);
  Logger::instance().debug("Plugin registered v2 animation parser for game=" + gid);
}

static void cb_v2_register_category(GmmRegistrationCtxV2 *ctx, const char *category) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->category_register(category);
}

static void cb_v2_register_categories(GmmRegistrationCtxV2 *ctx, const int *ids,
                                      const char *const *names, const int *parent_ids,
                                      size_t count) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  RegistrationSink::categories_register(
      bridge && bridge->current_plugin ? bridge->current_plugin : nullptr, ids, names,
      parent_ids, count);
}

static void cb_v2_register_tab(GmmRegistrationCtxV2 *ctx, const char *capability,
                               const char *display_name, const char *data_path,
                               const char *description, const char *protocol_handler,
                               const char *website_domain,
                               const char *supported_platforms,
                               const char *insert_before, const char *insert_after) {
  auto *bridge = static_cast<RegistrationBridge *>(ctx->user_data);
  if (!bridge || !bridge->current_plugin)
    return;

  bridge->sink->capability_register(capability, display_name, data_path, description,
                                    protocol_handler, website_domain,
                                    supported_platforms, insert_before, insert_after);
}

// v3 host services callbacks (gmm_abi_v3.h GmmHostServices).
static void v3_host_log(void *host_ctx, int level, const char *msg) {
  (void)host_ctx;
  if (!msg)
    return;
  switch (level) {
  case GMM_LOG_DEBUG:
    Logger::instance().debug(msg);
    break;
  case GMM_LOG_INFO:
    Logger::instance().info(msg);
    break;
  case GMM_LOG_WARN:
    Logger::instance().warn(msg);
    break;
  case GMM_LOG_ERROR:
    Logger::instance().error(msg);
    break;
  default:
    Logger::instance().info(msg);
    break;
  }
}

static const char *v3_host_resolve_path(void *host_ctx, const char *root,
                                        const char *rel) {
  (void)host_ctx;
  if (!root || !rel)
    return nullptr;
  const auto gf =
      engine::vfs::PathResolver(std::filesystem::path(root)).resolve(std::string(rel));
  if (!gf)
    return nullptr;
  const std::string s = gf->absolute().string();
  char *out           = static_cast<char *>(std::malloc(s.size() + 1));
  if (out)
    std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

PluginLoader::~PluginLoader() {
  unload_all();
}

// ---- v3 adapter: legacy v1 (body moved verbatim from the old if-branch) ----
static bool load_legacy_v1(void *handle, const std::string &path, PluginLoader &loader,
                           PluginInfo &info, uint64_t /*features*/) {
  auto register_fn = reinterpret_cast<void (*)(GmmRegistrationCtx *)>(
      engine::shared_library_symbol(handle, "gmm_register_v1"));
  if (!register_fn) {
    Logger::instance().error("Plugin missing gmm_register_v1: " + path);
    engine::unload_shared_library(handle);
    return false;
  }

  GmmRegistrationCtx ctx            = {};
  ctx.register_identity             = cb_register_identity;
  ctx.register_stage_claim          = cb_register_stage_claim;
  ctx.register_hook                 = cb_register_hook;
  ctx.register_order_encoding       = cb_register_order_encoding;
  ctx.register_deploy_strategy      = cb_register_deploy_strategy;
  ctx.register_tool                 = cb_register_tool;
  ctx.register_sort_provider        = cb_register_sort_provider;
  ctx.register_image_diff           = cb_register_image_diff;
  ctx.register_capability           = cb_register_capability;
  ctx.register_tab                  = cb_register_tab;
  ctx.register_meta                 = cb_register_meta;
  ctx.register_category             = cb_register_category;
  ctx.register_categories           = cb_register_categories;
  ctx.register_settings             = cb_register_settings;
  ctx.register_settings_tab         = cb_register_settings_tab;
  ctx.register_diagnostics          = cb_register_diagnostics;
  ctx.register_game_feature         = cb_register_game_feature;
  ctx.register_game_feature_data    = cb_register_game_feature_data;
  ctx.register_save_parser          = cb_register_save_parser;
  ctx.register_animation_parser     = cb_register_animation_parser;
  ctx.subscribe_event               = cb_subscribe_event;
  ctx.host_ui.fomod_wizard          = cb_fomod_wizard;
  ctx.register_wildcard_stage_claim = cb_register_wildcard_stage_claim;

  RegistrationBridge bridge;
  bridge.loader         = &loader;
  bridge.current_plugin = &info;

  RegistrationSink sink(loader, info);
  bridge.sink = &sink;

  ctx.user_data = &bridge;

  register_fn(&ctx);

  info.registered = true;
  loader.add_loaded_plugin(info);

  Logger::instance().debug("Plugin registered: " + info.game_display_name + " (" +
                           path + ", game=" + info.game_id +
                           ", appid=" + std::to_string(info.steam_appid) + ")");
  return true;
}

// ---- v3 adapter: legacy v2 (body moved verbatim from the old else-if branch) ----
static bool load_legacy_v2(void *handle, const std::string &path, PluginLoader &loader,
                           PluginInfo &info, uint64_t /*features*/) {
  auto register_fn = reinterpret_cast<void (*)(GmmRegistrationCtxV2 *)>(
      engine::shared_library_symbol(handle, "gmm_register_v2"));
  if (!register_fn) {
    Logger::instance().error("Plugin missing gmm_register_v2: " + path);
    engine::unload_shared_library(handle);
    return false;
  }

  RegistrationBridge bridge;
  bridge.loader         = &loader;
  bridge.current_plugin = &info;

  RegistrationSink sink(loader, info);
  bridge.sink = &sink;

  GmmRegistrationCtxV2 ctx          = {};
  ctx.user_data                     = &bridge;
  ctx.register_plugin               = cb_v2_register_plugin;
  ctx.register_settings             = cb_v2_register_settings;
  ctx.register_settings_tab         = cb_v2_register_settings_tab;
  ctx.register_requirements         = cb_v2_register_requirements;
  ctx.register_diagnostics          = cb_v2_register_diagnostics;
  ctx.register_game                 = cb_v2_register_game;
  ctx.register_game_feature         = cb_v2_register_game_feature;
  ctx.register_game_feature_data    = cb_v2_register_game_feature_data;
  ctx.register_hook                 = cb_v2_register_hook;
  ctx.register_order_encoding       = cb_v2_register_order_encoding;
  ctx.register_deploy_strategy      = cb_v2_register_deploy_strategy;
  ctx.register_file_mapper          = cb_v2_register_file_mapper;
  ctx.register_sort_provider        = cb_v2_register_sort_provider;
  ctx.register_stage_claim          = cb_v2_register_stage_claim;
  ctx.register_wildcard_stage_claim = cb_v2_register_wildcard_stage_claim;
  ctx.register_preview              = cb_v2_register_preview;
  ctx.register_tool                 = cb_v2_register_tool;
  ctx.register_modpage              = cb_v2_register_modpage;
  ctx.register_save_parser          = cb_v2_register_save_parser;
  ctx.register_animation_parser     = cb_v2_register_animation_parser;
  ctx.register_category             = cb_v2_register_category;
  ctx.register_categories           = cb_v2_register_categories;
  ctx.register_tab                  = cb_v2_register_tab;
  ctx.resolve_file                  = cb_v2_resolve_file;
  ctx.resolve_file_user_data        = nullptr;
  ctx.host_ui.fomod_wizard          = cb_fomod_wizard_v2;
  ctx.register_game_validator       = cb_v2_register_game_validator;
  ctx.register_game_variant         = cb_v2_register_game_variant;
  ctx.register_save_overlay         = cb_v2_register_save_overlay;

  register_fn(&ctx);

  info.registered = true;
  loader.add_loaded_plugin(info);

  Logger::instance().debug("Plugin (v2) registered: " + info.game_display_name + " (" +
                           path + ", game=" + info.game_id + ")");
  return true;
}

// ---- v3 adapter: OOP/ECS desc blob (gmm_entry) ----
static bool load_v3(void *handle, const std::string &path, PluginLoader &loader,
                    PluginInfo &info, uint64_t /*features*/) {
  auto entry_fn = reinterpret_cast<const GmmPluginDesc *(*)(const GmmHostServices *)>(
      engine::shared_library_symbol(handle, "gmm_entry"));
  if (!entry_fn) {
    Logger::instance().error("Plugin missing gmm_entry: " + path);
    engine::unload_shared_library(handle);
    return false;
  }

  GmmHostServices host = {};
  host.version         = GMM_API_VERSION;
  host.host_ctx        = nullptr;
  host.log             = v3_host_log;
  host.resolve_path    = v3_host_resolve_path;

  const GmmPluginDesc *desc = entry_fn(&host);
  if (!desc) {
    Logger::instance().error("Plugin gmm_entry returned null: " + path);
    engine::unload_shared_library(handle);
    return false;
  }

  // Version gate: the desc version and the abi version symbol must both be 3.
  auto version_fn = reinterpret_cast<uint32_t (*)()>(
      engine::shared_library_symbol(handle, "gmm_abi_version"));
  uint32_t plugin_abi = version_fn ? version_fn() : 0;
  if (plugin_abi != GMM_API_VERSION || desc->version != GMM_API_VERSION) {
    Logger::instance().error("ABI version mismatch: plugin=" +
                             std::to_string(plugin_abi) + " host supports 1, 2 and 3");
    engine::unload_shared_library(handle);
    return false;
  }

  if (desc->name)
    info.plugin_name = desc->name;
  if (desc->author)
    info.author = desc->author;
  if (desc->version_string)
    info.version = desc->version_string;
  if (desc->description)
    info.description = desc->description;

  info.v3_desc      = desc;
  info.v3_cap_count = desc->cap_count;
  info.v3_caps      = desc->caps;

  RegistrationBridge bridge;
  bridge.loader         = &loader;
  bridge.current_plugin = &info;

  RegistrationSink sink(loader, info);
  bridge.sink = &sink;

  // Capabilities: validate against the manifest and attach to the existing
  // registries. Unknown ids warn and drop; functional ids with a NULL invoke
  // refuse the whole desc.
  for (uint32_t i = 0; i < desc->cap_count; ++i) {
    const GmmCapabilityObject &c      = desc->caps[i];
    const char *cid                   = c.id ? c.id : "";
    const CapabilityManifestEntry *me = find_capability(cid);
    if (!me) {
      Logger::instance().warn("v3 plugin " + path + ": unknown capability id '" +
                              std::string(cid) + "' - dropped");
      continue;
    }
    if (me->kind == CapabilityKind::Functional && c.invoke == nullptr) {
      Logger::instance().error("v3 plugin " + path + ": functional capability '" +
                               std::string(cid) + "' has NULL invoke - refusing desc");
      engine::unload_shared_library(handle);
      return false;
    }
    if (c.version > me->payload_version) {
      Logger::instance().warn("v3 plugin " + path + ": capability '" +
                              std::string(cid) + "' payload version " +
                              std::to_string(c.version) + " newer than host " +
                              std::to_string(me->payload_version) + " - dropped");
      continue;
    }
    // Attach dispatch: feed the existing registries based on id.
    std::string gid = c.game_id ? c.game_id : "";
    if (gid.empty() && desc->game_count > 0 && desc->games)
      gid = desc->games[0].game_id ? desc->games[0].game_id : "";
    if (std::string(cid) == "detection.validator") {
      // Rescued validator: store on the plugin (v2-compatible slot) so the
      // instance-creation browse + secondary detection pass can consume it.
      info.game_validator_fn        = reinterpret_cast<void *>(c.invoke);
      info.game_validator_user_data = c.self;
    } else if (std::string(cid) == "tool") {
      // Feed PluginToolRegistry via the sink. tool_id defaults to game_id when
      // the cap carries no explicit id (the ABI cap has no payload field).
      std::string tid = gid.empty() ? std::string(cid) : gid;
      sink.tool_register(tid.c_str(), "tool", reinterpret_cast<void *>(c.invoke),
                         c.self, true);
    } else if (std::string(cid) == "preview.file") {
      // preview registry needs an extension; without payload we register
      // nothing here (the cap's invoke is still reachable via v3_caps).
    } else if (std::string(cid) == "event.subscribe") {
      // EventBus: store the invoke; consumers dispatch through v3_caps.
    }
    // Other ids: validated + accepted; consumers read v3_caps directly.
  }

  // Games: one record per GmmGameEntity, via the sink (same registries).
  for (uint32_t i = 0; i < desc->game_count; ++i) {
    const GmmGameEntity &g = desc->games[i];
    GameDescriptor gd;
    gd.provenance  = DescriptorProvenance::CompiledV3;
    gd.plugin_path = path;
    if (g.game_id)
      gd.id = g.game_id;
    if (g.display_name)
      gd.display_name = g.display_name;
    if (g.mods_subpath)
      gd.mods_subpath = g.mods_subpath;
    if (g.steam_appid_count > 0 && g.steam_appids)
      gd.steam_appid = g.steam_appids[0];
    if (g.gog_id)
      gd.gog_id = g.gog_id;
    if (g.epic_namespace)
      gd.epic_namespace = g.epic_namespace;
    if (g.nexus_domain)
      gd.nexus_domain = g.nexus_domain;
    const std::string dup_key = g.game_id ? g.game_id : "";
    sink.add_entity(gd, dup_key);
  }

  info.registered = true;
  loader.add_loaded_plugin(info);

  Logger::instance().debug("Plugin (v3) registered: " + info.game_display_name + " (" +
                           path + ", game=" + info.game_id +
                           ", games=" + std::to_string(desc->game_count) +
                           ", caps=" + std::to_string(desc->cap_count) + ")");
  return true;
}

namespace {
  struct ApiSurface {
    uint32_t version;
    const char *entry_symbol;
    bool (*load)(void *handle, const std::string &path, PluginLoader &loader,
                 PluginInfo &info, uint64_t features);
  };
  static const ApiSurface kSurfaces[] = {
      {1, "gmm_register_v1", load_legacy_v1},
      {2, "gmm_register_v2", load_legacy_v2},
      {3, "gmm_entry", load_v3},
  };
  static const ApiSurface *surface_for(uint32_t v) {
    for (const auto &s : kSurfaces)
      if (s.version == v)
        return &s;
    return nullptr;
  }
}  // namespace

bool PluginLoader::load_plugin(const std::string &path) {
  if (is_loaded(path)) {
    Logger::instance().warn("Plugin already loaded: " + path);
    return true;
  }

  if (is_disabled(std::filesystem::path(path).filename().string())) {
    Logger::instance().debug("Plugin disabled in settings, skipping: " + path);
    return false;
  }

  void *handle = engine::load_shared_library(path);
  if (!handle) {
    Logger::instance().error("Failed to load plugin: " + path + " - " +
                             engine::dlerror_message());
    return false;
  }

  // Check ABI version first (authoritative).
  auto version_fn = reinterpret_cast<uint32_t (*)()>(
      engine::shared_library_symbol(handle, "gmm_abi_version"));
  if (!version_fn) {
    Logger::instance().error("Plugin missing gmm_abi_version: " + path);
    engine::unload_shared_library(handle);
    return false;
  }

  uint32_t plugin_abi = version_fn();

  // Probe for the optional gmm_abi_features symbol.
  uint64_t plugin_features = 0;
  if (auto features_fn = reinterpret_cast<uint64_t (*)()>(
          engine::shared_library_symbol(handle, "gmm_abi_features"))) {
    plugin_features = features_fn();
  }

  const ApiSurface *surf = surface_for(plugin_abi);
  if (!surf) {
    Logger::instance().error("ABI version mismatch: plugin=" +
                             std::to_string(plugin_abi) + " host supports 1, 2 and 3");
    engine::unload_shared_library(handle);
    return false;
  }

  PluginInfo info;
  info.path              = path;
  info.game_id           = std::filesystem::path(path).stem().string();
  info.game_display_name = info.game_id;
  info.abi_version       = plugin_abi;
  info.features          = plugin_features;
  info.loaded            = true;
  info.handle            = handle;

  if (!surf->load(handle, path, *this, info, plugin_features)) {
    return false;
  }
  return true;
}

bool PluginLoader::load_directory(const std::string &dir_path) {
  namespace fs = std::filesystem;

  if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
    Logger::instance().warn("Plugin directory not found: " + dir_path);
    return false;
  }

  int loaded = 0;

  // Deterministic load order (v3 P1): directory_iterator order is
  // filesystem-dependent, so colliding game_ids would resolve differently
  // across machines. Sort by filename; "first registrant wins" becomes
  // first in filename order and the whole registry state is reproducible.
  std::vector<fs::path> entries;
  for (const auto &entry : fs::directory_iterator(dir_path))
    if (entry.is_regular_file())
      entries.push_back(entry.path());
  std::sort(entries.begin(), entries.end());

  for (const auto &path : entries) {
    auto ext = path.extension().string();

    // Platform-appropriate shared library extensions. CMake MODULE
    // libraries keep the ".so" suffix even on macOS, so Apple accepts
    // both spellings (dlopen loads either).
#if defined(_WIN32)
    if (ext == ".dll") {
      if (load_plugin(path.string()))
        loaded++;
    }
#else
    if (ext == ".so" || ext == ".dylib") {
      if (load_plugin(path.string()))
        loaded++;
    }
#endif

    // Python plugins - always attempted regardless of OS
    if (ext == ".py") {
      if (is_disabled(path.filename().string())) {
        Logger::instance().debug("Plugin disabled in settings, skipping: " +
                                 path.string());
        continue;
      }
      if (python_load_plugin(this, path.string()))
        loaded++;
    }
  }

  Logger::instance().debug("Loaded " + std::to_string(loaded) + " plugins from " +
                           dir_path);

  std::string list_str;
  for (size_t i = 0; i < plugins_.size(); ++i) {
    if (i > 0)
      list_str += ", ";
    list_str += plugins_[i].game_display_name;
  }
  Logger::instance().debug("Loaded plugins: [" + list_str + "]");

  // Evaluate plugin dependency requirements now that every plugin is loaded.
  // check_requirements() calls each registered provider and returns the
  // requirements that are not satisfied by the loaded plugin/game set.
  auto unmet = RequirementsRegistry::instance().check_requirements(plugins_);
  for (const auto &u : unmet) {
    std::string msg = u.message.empty()
                          ? ("requires " + (u.type.empty() ? "dependency" : u.type) +
                             (u.name.empty() ? "" : " '" + u.name + "'"))
                          : u.message;
    Logger::instance().warn("Plugin requirement unmet (" + u.plugin_path + "): " + msg);
  }
  if (!unmet.empty()) {
    Logger::instance().warn(std::to_string(unmet.size()) +
                            " plugin requirement(s) unmet across loaded plugins");
  }

  // Nothing to finalise here: every game plugin is loaded into this process
  // regardless of which game is on screen, so picking a category set now
  // would mean picking it for a game nobody is looking at. Plugin-declared
  // categories are already recorded per game (cb_register_categories), and
  // the set for the game actually in front of the user is applied when that
  // game's instance is activated (Category::Factory::activate_game).

  return loaded > 0;
}

bool PluginLoader::is_loaded(const std::string &path) const {
  for (const auto &p : plugins_) {
    if (p.path == path)
      return true;
  }
  return false;
}

bool PluginLoader::is_disabled(const std::string &filename) const {
  for (const auto &name : disabled_plugins_) {
    if (name == filename)
      return true;
  }
  return false;
}

void PluginLoader::add_loaded_plugin(PluginInfo info) {
  plugins_.push_back(std::move(info));
}

void PluginLoader::collect_diagnostics(const std::string &game_id,
                                       PluginDb::Database &db) {
  DiagnosticsRegistry::instance().collect(game_id, db);
}

void PluginLoader::unload_all() {
  for (auto &p : plugins_) {
    // Drop this plugin's event subscriptions BEFORE dlclose so no bus
    // callback can ever run against unloaded .so code.
    EventBus::instance().clear_source(p.path);
    // Clear hooks registered by this plugin.
    hook_registry_.clear_plugin_hooks(p.path);
    // Clear v2 behavior-injection hooks registered by this plugin so the
    // v2 HookRegistry singleton never holds a dangling function pointer.
    ::HookRegistry::instance().clear_plugin(p.path.c_str());
    // Clear v2 order-encoding / deploy-strategy callbacks registered by
    // this plugin (dangling after dlclose).
    OrderEncodingRegistry::instance().clear_plugin(p.path);
    DeployStrategyRegistry::instance().clear_plugin(p.path);
    // Clear v2 preview generators registered by this plugin so the
    // Registry never holds a dangling function pointer.
    ui::preview::Registry::instance().clear_plugin(p.path);
    // Clear v2 diagnose providers registered by this plugin so the
    // DiagnoseRegistry never holds a dangling function pointer.
    DiagnoseRegistry::instance().clear_plugin(p.path);
    // Clear v2 file mappers registered by this plugin so the
    // FileMapperRegistry never holds a dangling function pointer.
    FileMapperRegistry::instance().clear_plugin(p.path);
    // Clear v2 requirement providers registered by this plugin so the
    // RequirementsRegistry never holds a dangling function pointer.
    RequirementsRegistry::instance().clear_plugin(p.path);
    // Clear save parsers registered by this plugin so the
    // SaveParserRegistry never holds a dangling ABI function pointer.
    SaveParserRegistry::instance().clear_plugin(p.path);
    // Clear v2 IPluginTool callbacks registered by this plugin so the
    // ToolRegistry never holds a dangling function pointer.
    PluginToolRegistry::instance().clear_plugin(p.path);
    if (p.handle) {
      engine::unload_shared_library(p.handle);
      p.handle = nullptr;
    }
  }
  plugins_.clear();
}

std::string PluginLoader::display_name_for(const std::string &game_id) const {
  for (const auto &p : plugins_) {
    if (p.game_id == game_id)
      return p.game_display_name;
  }
  // Fallback: resolve via fuzzy match
  auto resolved = resolve_game_id(game_id);
  if (resolved != game_id)
    return display_name_for(resolved);
  return game_id;
}

std::string PluginLoader::resolve_game_id(const std::string &game_id) const {
  // Exact match - fast path
  for (const auto &p : plugins_)
    if (p.game_id == game_id)
      return game_id;

  // Fuzzy: check if any plugin's game_id contains the query or vice versa
  // (handles shortname→fullname renames like "isaac" ↔
  // "TheBindingOfIsaacRebirth")
  std::string q_lower;
  for (char c : game_id)
    q_lower += static_cast<char>(std::tolower(c));

  for (const auto &p : plugins_) {
    std::string p_lower;
    for (char c : p.game_id)
      p_lower += static_cast<char>(std::tolower(c));

    if (p_lower.find(q_lower) != std::string::npos ||
        q_lower.find(p_lower) != std::string::npos)
      return p.game_id;
  }

  // Fuzzy: try normalizing display name to instance-name format
  // (spaces → underscores, remove illgal chars)
  for (const auto &p : plugins_) {
    std::string norm;
    for (char c : p.game_display_name) {
      if (c == ' ')
        norm += '_';
      else
        norm += c;
    }
    if (norm == game_id)
      return p.game_id;
  }

  return game_id;  // no match - return as-is
}

bool PluginLoader::validate_game_dir_impl(const std::string &game_id,
                                          const std::filesystem::path &dir) const {
  bool any = false;
  bool ok  = true;
  for (const auto &p : plugins_) {
    // v2 validator
    if (p.game_validator_fn && (p.game_id == game_id || p.game_id.empty())) {
      any     = true;
      auto fn = reinterpret_cast<GmmLooksValidFn>(p.game_validator_fn);
      if (!fn(dir.string().c_str(), p.game_validator_user_data))
        ok = false;
    }
    // v3 detection.validator capability
    if (p.v3_caps) {
      const auto *caps = static_cast<const GmmCapabilityObject *>(p.v3_caps);
      for (uint32_t i = 0; i < p.v3_cap_count; ++i) {
        const auto &c = caps[i];
        if (!c.id || std::string(c.id) != "detection.validator")
          continue;
        std::string cid = c.game_id ? c.game_id : "";
        if (!cid.empty() && cid != game_id)
          continue;
        if (!c.invoke)
          continue;
        any = true;
        struct Args {
          const char *dir;
        } args{dir.string().c_str()};
        int result = 0;
        struct Out {
          int result;
        } out{0};
        if (c.invoke(c.self, 1 /*GMM_VALIDATE_DIR*/, &args, &out) == 0) {
          result = out.result;
        } else {
          // Fallback convention: args is the dir string, out is int*.
          int r   = 0;
          void *o = &r;
          // Re-invoke with alternate convention only if first produced nothing?
          // Keep first result.
          result = r;
        }
        if (!result)
          ok = false;
      }
    }
  }
  if (!any)
    return true;
  return ok;
}

}  // namespace engine
