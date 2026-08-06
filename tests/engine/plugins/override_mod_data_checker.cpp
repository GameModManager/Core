// P1.2 test fixture — a plugin that OVERRIDES the Skyrim plugin's
// mod_data_checker AND game_plugins via the register_game_feature C ABI.
// Built only for game_feature_registry_test: it registers its allow-sets and
// its vanilla-plugin band for the Skyrim game at a priority above the game
// plugin's own baseline (0), so the mod list (ModScanner::scan_dir ->
// invalid_data) must accept a mod whose only content is "customstuff/", and
// native_plugins_csv() must return the override's band — proof that both
// overrides show without any engine change. Not shipped; the app never loads
// it.

#include "gmm_abi_v1.h"

#include <stddef.h>

static const char* const OVERRIDE_DIRS[] = {
    "customstuff",
};

static const char* const OVERRIDE_EXTS[] = {
    "custoext",
};

static const char* const OVERRIDE_PLUGINS[] = {
    "VanillaOverride.esm",
    "AlsoVanilla.esm",
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

        ctx->register_game_feature(ctx,
            "SkyrimSpecialEdition",       /* game_id — override Skyrim's band */
            "game_plugins",
            100,                          /* priority — above the game's own (0) */
            OVERRIDE_PLUGINS,
            sizeof(OVERRIDE_PLUGINS) / sizeof(OVERRIDE_PLUGINS[0]),
            NULL,
            0);
    }
}

extern "C" uint32_t gmm_abi_version(void) {
    return GMM_ABI_VERSION;
}
