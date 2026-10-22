// v3 P1 determinism fixture - registers the SAME game_id as its sibling
// target. Both targets compile from this file with a different
// GMM_COLLIDE_DISPLAY_NAME; the determinism test loads both from one
// directory and asserts the alphabetically-first filename wins game
// support (sorted directory load, spec 4.4). Built only for
// plugin_n_records_test; never shipped.

#include "gmm_abi_v2.h"

#ifndef GMM_COLLIDE_DISPLAY_NAME
#define GMM_COLLIDE_DISPLAY_NAME "Collide Unknown"
#endif

extern "C" void gmm_register_v2(GmmRegistrationCtxV2 *ctx) {
  if (!ctx || !ctx->register_game)
    return;

  GmmGameInfo info  = {};
  info.game_id      = "CollideGame";
  info.display_name = GMM_COLLIDE_DISPLAY_NAME;
  ctx->register_game(ctx, info);
}

extern "C" uint32_t gmm_abi_version(void) {
  return GMM_ABI_VERSION;
}
