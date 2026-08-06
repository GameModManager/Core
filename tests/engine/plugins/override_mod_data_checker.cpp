// P1.2 test fixture — a plugin that OVERRIDES the Skyrim plugin's
// mod_data_checker, game_plugins, AND script_extender via the
// register_game_feature / register_game_feature_data C ABI, and registers a
// bsa_invalidation feature Skyrim's own plugin does NOT provide (proving the
// 9th feature type registers through the same surface). Built only for
// game_feature_registry_test: it registers its allow-sets and its vanilla
// plugin band for the Skyrim game at a priority above the game plugin's own
// baseline (0), so the mod list (ModScanner::scan_dir -> invalid_data) must
// accept a mod whose only content is "customstuff/", and
// native_plugins_csv() must return the override's band — proof that both
// overrides show without any engine change. Not shipped; the app never loads
// it.

#include "gmm_abi_v1.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

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

static const char* const OVERRIDE_SE_KEYS[] = {
    "binary",
    "plugin_path",
    "loader_name",
    "savegame_extension",
};

static const char* const OVERRIDE_SE_VALS[] = {
    "superse_loader.exe",
    "superse/plugins",
    "superse_loader.exe",
    "sse",
};

static const char* const OVERRIDE_BSA_KEYS[] = {
    "bsa_name",
    "bsa_version",
};

static const char* const OVERRIDE_BSA_VALS[] = {
    "CustomInvalidation.bsa",
    "0x68",
};

// P1.3 — subscribe to host events through the ABI. The fixture logs received
// events to the file named by GMM_TEST_EVENTS_LOG (set by the test); the
// subscription proves a C plugin reaches the host event bus end-to-end.
static void on_test_event(const char* event_id,
                          const char* json_payload,
                          void* user_data) {
    (void)user_data;
    const char* log = getenv("GMM_TEST_EVENTS_LOG");
    if (!log) return;
    FILE* f = fopen(log, "a");
    if (!f) return;
    fprintf(f, "%s %s\n", event_id, json_payload);
    fclose(f);
}

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
    if (ctx->register_game_feature_data) {
        ctx->register_game_feature_data(ctx,
            "SkyrimSpecialEdition",       /* game_id — override Skyrim's SKSE */
            "script_extender",
            100,                          /* priority — above the game's own (0) */
            OVERRIDE_SE_KEYS,
            OVERRIDE_SE_VALS,
            sizeof(OVERRIDE_SE_KEYS) / sizeof(OVERRIDE_SE_KEYS[0]));

        ctx->register_game_feature_data(ctx,
            "SkyrimSpecialEdition",       /* game_id — Skyrim registers none */
            "bsa_invalidation",
            100,                          /* priority — only registration */
            OVERRIDE_BSA_KEYS,
            OVERRIDE_BSA_VALS,
            sizeof(OVERRIDE_BSA_KEYS) / sizeof(OVERRIDE_BSA_KEYS[0]));
    }
    if (ctx->subscribe_event) {
        ctx->subscribe_event(ctx, "mod_installed", on_test_event, NULL);
        ctx->subscribe_event(ctx, "game_finished", on_test_event, NULL);
    }
}

extern "C" uint32_t gmm_abi_version(void) {
    return GMM_ABI_VERSION;
}
