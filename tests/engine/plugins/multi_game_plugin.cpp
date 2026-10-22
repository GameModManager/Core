// v3 P1 test fixture - registers TWO games via register_game (spec 6.2
// N-records). Old engine behavior: the second call overwrites the first
// (single-record storage, last wins) and game_plugins() returns one row.
// New behavior: two records, game_plugins() returns one row per record.
// Built only for plugin_n_records_test; never shipped.

#include "gmm_abi_v2.h"

extern "C" void gmm_register_v2(GmmRegistrationCtxV2 *ctx) {
  if (!ctx || !ctx->register_game)
    return;

  GmmGameInfo first  = {};
  first.game_id      = "MultiGameAlpha";
  first.display_name = "Multi Game Alpha";
  first.steam_appid  = 480;
  ctx->register_game(ctx, first);

  GmmGameInfo second  = {};
  second.game_id      = "MultiGameBeta";
  second.display_name = "Multi Game Beta";
  second.steam_appid  = 481;
  ctx->register_game(ctx, second);
}

extern "C" uint32_t gmm_abi_version(void) {
  return GMM_ABI_VERSION;
}
