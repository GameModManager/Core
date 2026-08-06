// P1.5 test fixture — a plugin that declares a TYPED settings tab via the new
// register_settings_tab C ABI entry: one setting of each supported type
// (bool / int / string / choice) plus a plain register_settings key that the
// tab does NOT declare (so the Plugins-tab info pane keeps showing that one as
// a raw key:value row while the declared ones move to the typed tab). Built
// only for settings_plugins_tab_test; not shipped; the app never loads it.

#include "gmm_abi_v1.h"

#include <stdint.h>
#include <string.h>

extern "C" void gmm_register_v1(GmmRegistrationCtx* ctx) {
    if (ctx->register_identity) {
        ctx->register_identity(ctx, 0, NULL, NULL, NULL, "Settings Tab Fixture",
                               NULL, NULL, NULL);
    }
    if (ctx->register_category) {
        ctx->register_category(ctx, "Settings Page");
    }

    // One plain register_settings key the typed tab does NOT declare — it
    // must keep rendering as a key:value row in the Plugins-tab info pane.
    if (ctx->register_settings) {
        static const char* settings_keys[] = {"plain_legacy_key"};
        static const char* settings_values[] = {"legacy_value"};
        ctx->register_settings(ctx, settings_keys, settings_values, 1);
    }

    // The typed settings tab: one of each supported type.
    if (ctx->register_settings_tab) {
        static const char* tab_keys[] = {
            "show_previews",   // bool
            "max_threads",     // int
            "mod_name_prefix", // string
            "install_mode",    // choice
        };
        static const char* tab_types[] = {
            "bool", "int", "string", "choice",
        };
        static const char* tab_defaults[] = {
            "1", "4", "mod_", "Full",
        };
        static const char* tab_options[] = {
            NULL,       // bool: none
            "1:8",      // int range
            NULL,       // string: none
            "Full\nCompact\nMinimal",  // choice candidates
        };
        ctx->register_settings_tab(ctx, "Fixture Settings", tab_keys, tab_types,
                                   tab_defaults, tab_options, 4);
    }
}

extern "C" uint32_t gmm_abi_version(void) {
    return GMM_ABI_VERSION;
}
