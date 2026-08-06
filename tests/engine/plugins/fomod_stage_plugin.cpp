// P1.4 test fixture — a plugin that CLAIMS the "Fomod" install-template stage
// via register_stage_claim and runs it through the host UI bridge
// (GmmHostUi::fomod_wizard): the engine's Qt-free FomodStage does all the
// install work host-side (detect fomod/, parse ModuleConfig.xml, apply the
// chosen options to the staging dir, flatten), the wizard is the host's
// fomod_query_cb, and the outcome comes back to the plugin as JSON. The
// fixture logs every step to the file named by GMM_TEST_FOMOD_LOG (set by the
// test); the log proves the plugin's claim won over the core stage and the
// bridge ran end-to-end. Built only for fomod_stage_plugin_test; not shipped;
// the app never loads it.

#include "gmm_abi_v1.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The host UI bridge pointer, cached at register time. Only the FUNCTION
// POINTER is cached (stable host code, valid for the host's lifetime) —
// never the GmmRegistrationCtx (host storage, valid only during
// gmm_register_v1).
static GmmFomodWizardFn g_wizard = NULL;

static void log_line(const char* line) {
    const char* log = getenv("GMM_TEST_FOMOD_LOG");
    if (!log) return;
    FILE* f = fopen(log, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static int fomod_stage_handler(GmmModHandle mod, GmmInstanceHandle instance,
                               GmmConflictIndexHandle conflicts, GmmProfileHandle profile,
                               void* user_data) {
    (void)conflicts;
    (void)profile;
    (void)user_data;

    // The plugin owns the stage: log what it can see from the opaque handles.
    char line[512];
    const char* gid = gmm_instance_game_id(instance);
    const size_t n = gmm_mod_file_count(mod);
    const char* root = n > 0 ? gmm_mod_file_at(mod, 0).relative_path : "";
    snprintf(line, sizeof line, "stage_claimed game=%s files=%zu root=%s",
             gid, n, root);
    log_line(line);

    // Run the install through the host UI bridge — the plugin never touches
    // widgets, it only requests the host's FOMOD wizard for this mod.
    if (!g_wizard) {
        log_line("no_ui_bridge");
        return 0;
    }
    char json[4096];
    if (!g_wizard(mod, json, sizeof json)) {
        log_line("wizard_failed");
        return 0;
    }

    snprintf(line, sizeof line, "wizard_outcome %s", json);
    log_line(line);

    // Interpret the outcome minimally (substring match on the documented keys).
    if (strstr(json, "\"outcome\":\"not_fomod\"")) return 1;  // pass through
    if (strstr(json, "\"outcome\":\"canceled\"")) return 0;   // wizard cancel
    if (strstr(json, "\"outcome\":\"failed\"")) return 0;     // install error
    // installed — log the final mod folder name.
    const char* fn = strstr(json, "\"final_name\":\"");
    if (fn) {
        fn += strlen("\"final_name\":\"");
        char name[256];
        size_t i = 0;
        while (fn[i] && fn[i] != '"' && i < sizeof name - 1) {
            name[i] = fn[i];
            ++i;
        }
        name[i] = '\0';
        snprintf(line, sizeof line, "installed_as %s", name);
        log_line(line);
    }
    return 1;
}

extern "C" void gmm_register_v1(GmmRegistrationCtx* ctx) {
    if (ctx->host_ui.fomod_wizard) {
        g_wizard = ctx->host_ui.fomod_wizard;
    }
    if (ctx->register_stage_claim) {
        // Claims the "Fomod" stage of the install template for its own game
        // (register_stage_claim keys by the plugin's own game_id = the .so
        // stem) at priority 100 — above the core baseline so the claim wins in
        // main_window's claim_for.
        ctx->register_stage_claim(ctx, "Fomod", fomod_stage_handler, 100);
    }
}

extern "C" uint32_t gmm_abi_version(void) {
    return GMM_ABI_VERSION;
}
