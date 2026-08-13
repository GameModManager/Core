#include "engine/index/conflict_index.h"
#include "engine/model/profile.h"
#include "engine/model/mod.h"
#include "engine/deploy/order_hook.h"
#include "engine/deploy/deploy_ledger.h"
#include "engine/registry/stage_registry.h"
#include "engine/registry/hook_registry.h"
#include "engine/pipeline/pipeline.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

using namespace engine;
using namespace std::chrono;

static const int MOD_COUNT = 3000;
static const int FILES_PER_MOD = 50;
static const int CONFLICT_RATE = 10;  // 10% of files conflict with another mod

static std::string mod_id(int i) {
    return "mod_" + std::to_string(i);
}

static std::string file_path(int mod_index, int file_index) {
    // Some files shared across mods (conflicts), some unique
    if (file_index % CONFLICT_RATE == 0) {
        // Shared path — many mods contribute to this path
        return "Data/shared/file_" + std::to_string(file_index % 100) + ".esp";
    }
    return "Data/" + mod_id(mod_index) + "/file_" + std::to_string(file_index) + ".esp";
}

TEST_CASE("benchmark", "[perf]") {
    printf("=== Performance Benchmark (§14 targets) ===\n");
    printf("Mods: %d, Files/mod: %d, Conflict rate: ~%d%%\n\n",
           MOD_COUNT, FILES_PER_MOD, CONFLICT_RATE);

    // === ConflictIndex: bulk insert ===
    {
        ConflictIndex index;
        auto start = high_resolution_clock::now();

        for (int i = 0; i < MOD_COUNT; ++i) {
            for (int f = 0; f < FILES_PER_MOD; ++f) {
                index.add_file(file_path(i, f), mod_id(i), static_cast<uint32_t>(i));
            }
        }

        auto end = high_resolution_clock::now();
        auto ms = duration_cast<milliseconds>(end - start).count();
        printf("[ConflictIndex] Bulk insert %d mods x %d files: %ld ms\n",
               MOD_COUNT, FILES_PER_MOD, ms);

        // §14 target: indexed data structures, not linear scans
        // Verify lookups are fast
        start = high_resolution_clock::now();
        for (int i = 0; i < 1000; ++i) {
            index.winner(file_path(i % MOD_COUNT, 0));
        }
        end = high_resolution_clock::now();
        ms = duration_cast<microseconds>(end - start).count();
        printf("[ConflictIndex] 1000 winner lookups: %ld µs (avg %.1f µs each)\n",
               ms, ms / 1000.0);
    }

    // === ConflictIndex: remove_mod (single mod, should be O(files_in_mod)) ===
    {
        ConflictIndex index;
        for (int i = 0; i < MOD_COUNT; ++i) {
            for (int f = 0; f < FILES_PER_MOD; ++f) {
                index.add_file(file_path(i, f), mod_id(i), static_cast<uint32_t>(i));
            }
        }

        auto start = high_resolution_clock::now();
        index.remove_mod(mod_id(MOD_COUNT / 2));  // remove a middle mod
        auto end = high_resolution_clock::now();
        auto us = duration_cast<microseconds>(end - start).count();
        printf("[ConflictIndex] remove_mod (single mod, ~%d files): %ld µs\n",
               FILES_PER_MOD, us);
    }

    // === Profile: add + toggle + move ===
    {
        Profile profile;
        auto start = high_resolution_clock::now();

        for (int i = 0; i < MOD_COUNT; ++i) {
            profile.add_mod(mod_id(i), true);
        }

        auto end = high_resolution_clock::now();
        auto ms = duration_cast<milliseconds>(end - start).count();
        printf("[Profile] Add %d mods: %ld ms\n", MOD_COUNT, ms);

        // Toggle a single mod — §14 target: <100ms
        start = high_resolution_clock::now();
        profile.set_enabled(mod_id(MOD_COUNT / 2), false);
        end = high_resolution_clock::now();
        auto us = duration_cast<microseconds>(end - start).count();
        printf("[Profile] Toggle single mod: %ld µs (target: <100ms)\n", us);

        // Move (reorder) — §14 target: <100ms
        start = high_resolution_clock::now();
        profile.move_mod(mod_id(100), static_cast<uint32_t>(MOD_COUNT - 10));
        end = high_resolution_clock::now();
        us = duration_cast<microseconds>(end - start).count();
        printf("[Profile] Move single mod (reorder): %ld µs (target: <100ms)\n", us);

        // enabled_in_order — should be fast
        start = high_resolution_clock::now();
        auto ordered = profile.enabled_in_order();
        end = high_resolution_clock::now();
        us = duration_cast<microseconds>(end - start).count();
        printf("[Profile] enabled_in_order (%zu enabled): %ld µs\n",
               ordered.size(), us);
    }

    // === DeployLedger: diff after single toggle ===
    {
        DeployLedger ledger;
        for (int i = 0; i < MOD_COUNT; ++i) {
            for (int f = 0; f < FILES_PER_MOD; ++f) {
                ledger.record_deploy(file_path(i, f), mod_id(i),
                    static_cast<uint32_t>(i));
            }
        }

        // Build new winners after removing one mod
        std::unordered_map<std::string, std::string> new_winners;
        for (int i = 0; i < MOD_COUNT; ++i) {
            if (i == MOD_COUNT / 2) continue;  // skip the toggled mod
            for (int f = 0; f < FILES_PER_MOD; ++f) {
                new_winners[file_path(i, f)] = mod_id(i);
            }
        }

        auto start = high_resolution_clock::now();
        auto diff_result = ledger.diff(new_winners);
        auto end = high_resolution_clock::now();
        auto ms = duration_cast<milliseconds>(end - start).count();
        printf("[DeployLedger] diff (3000 mods, single toggle): %ld ms\n", ms);
        printf("[DeployLedger] Paths needing redeployment: %zu\n", diff_result.size());
    }

    // === StageRegistry: register + lookup ===
    {
        StageRegistry reg;
        auto start = high_resolution_clock::now();

        for (int i = 0; i < MOD_COUNT; ++i) {
            reg.register_claim(mod_id(i), "deploy",
                [](Mod&, PipelineContext&) { return true; },
                i % 10, mod_id(i));
        }

        auto end = high_resolution_clock::now();
        auto ms = duration_cast<milliseconds>(end - start).count();
        printf("[StageRegistry] Register %d claims: %ld ms\n", MOD_COUNT, ms);

        start = high_resolution_clock::now();
        for (int i = 0; i < 1000; ++i) {
            reg.get_handler(mod_id(i % MOD_COUNT), "deploy");
        }
        end = high_resolution_clock::now();
        auto us = duration_cast<microseconds>(end - start).count();
        printf("[StageRegistry] 1000 lookups: %ld µs (avg %.1f µs each)\n",
               us, us / 1000.0);
    }

    // === HookRegistry: register + fire ===
    {
        HookRegistry reg;
        int fire_count = 0;

        auto start = high_resolution_clock::now();
        for (int i = 0; i < MOD_COUNT; ++i) {
            reg.register_hook("skyrimse.resolve.post",
                [&](Mod&, PipelineContext&) { fire_count++; },
                i % 10, mod_id(i));
        }

        auto end = high_resolution_clock::now();
        auto ms = duration_cast<milliseconds>(end - start).count();
        printf("[HookRegistry] Register %d hooks: %ld ms\n", MOD_COUNT, ms);

        start = high_resolution_clock::now();
        Mod mod;
        PipelineContext ctx;
        reg.fire("skyrimse.resolve.post", mod, ctx);
        end = high_resolution_clock::now();
        ms = duration_cast<milliseconds>(end - start).count();
        printf("[HookRegistry] Fire %d hooks: %ld ms (all should run)\n",
               fire_count, ms);
    }
}
