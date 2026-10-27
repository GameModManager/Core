// Test fixture: a plugin that registers a game_id. When compiled with
// GMM_IS_TOOL, it declares itself a "Tool" (category registered before
// game) and must never receive game_support. Otherwise it is an untyped
// game-support provider and may receive game_support=true.
//
// Built as two targets (see tests/CMakeLists.txt): gmm_tool_first_plugin
// (GMM_IS_TOOL) and gmm_game_support_plugin (default). The test loads the
// Tool first, then the GameSupport, and asserts ownership lands on the
// GameSupport record.

#include "gmm_abi_v2.h"

extern "C" void gmm_register_v2(GmmRegistrationCtxV2 *ctx) {
  if (!ctx || !ctx->register_game)
    return;

#ifdef GMM_IS_TOOL
  if (ctx->register_category)
    ctx->register_category(ctx, "Tool");
#endif

  GmmGameInfo info  = {};
  info.game_id      = "ToolFirstGame";
  info.display_name = "Tool First Game";
  info.steam_appid  = 480;
  info.nexus_domain = "toolfirstgame";
  ctx->register_game(ctx, info);
}

extern "C" uint32_t gmm_abi_version(void) {
  return GMM_ABI_VERSION;
}
