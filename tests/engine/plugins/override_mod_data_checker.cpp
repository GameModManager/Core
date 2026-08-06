// P1.2 test fixture — a plugin that OVERRIDES the Skyrim plugin's
// mod_data_checker via the register_game_feature C ABI. Built only for
// game_feature_registry_test: it registers its allow-sets for the Skyrim
// game at a priority above the game plugin's own baseline (0), so the mod
// list (ModScanner::scan_dir -> invalid_data) must accept a mod whose only
// content is "customstuff/" — proof that the override shows without any
// engine change. Not shipped; the app never loads it.

#include "gmm_abi_v1.h"

#include <stddef.h>

static const char* const OVERRIDE_DIRS[] = {
    "customstuff",
};

static const char* const OVERRIDE_EXTS[] = {
    "custoext",
};

extern "C" void gmm_register_v1(GmmRegistrationCtx* ctx) {
    if (ctx->register_game_feature) {
        ctx->register_game_feature(ctx,
            "SkyrimSpecialEdition",       /* game_id — override Skyrim's checker */
            "mod_data_checker",
            100,                          /* priority — above the game's own (0) */
            OVERRIDE_DIRS,
            sizeof(OVERRIDE_DIRS) / sizeof(OVERRIDE_DIRS[0]),
            OVERRIDE_EXTS,
            sizeof(OVERRIDE_EXTS) / sizeof(OVERRIDE_EXTS[0]));
    }
}

extern "C" uint32_t gmm_abi_version(void) {
    return GMM_ABI_VERSION;
}
