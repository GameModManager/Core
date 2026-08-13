#include "engine/registry/stage_registry.h"
#include "engine/registry/hook_registry.h"
#include "engine/model/mod.h"
#include "engine/pipeline/pipeline.h"

#include <cstdio>
#include <string>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

static void test_stage_registry_basic() {
    StageRegistry reg;

    // No claims yet
    REQUIRE(!reg.has_claim("skyrimse", "deploy"));
    REQUIRE(!reg.get_handler("skyrimse", "deploy"));

    // Register a claim
    bool called = false;
    reg.register_claim("skyrimse", "deploy",
        [&](Mod&, PipelineContext&) -> bool { called = true; return true; },
        0, "test_plugin");

    REQUIRE(reg.has_claim("skyrimse", "deploy"));
    REQUIRE(!reg.has_claim("skyrimse", "launch"));
    REQUIRE(!reg.has_claim("isaac", "deploy"));

    // Get the handler and call it
    auto handler = reg.get_handler("skyrimse", "deploy");
    REQUIRE(handler);

    Mod mod;
    PipelineContext ctx;
    bool result = handler(mod, ctx);
    REQUIRE(result);
    REQUIRE(called);

    printf("  PASS: stage_registry_basic\n");
}

static void test_stage_registry_priority() {
    StageRegistry reg;

    int last_priority = -1;

    // Register two claims for the same stage — lower priority first
    reg.register_claim("skyrimse", "deploy",
        [&](Mod&, PipelineContext&) -> bool { last_priority = 1; return true; },
        1, "low_priority");

    reg.register_claim("skyrimse", "deploy",
        [&](Mod&, PipelineContext&) -> bool { last_priority = 10; return true; },
        10, "high_priority");

    auto handler = reg.get_handler("skyrimse", "deploy");
    REQUIRE(handler);

    Mod mod;
    PipelineContext ctx;
    handler(mod, ctx);
    REQUIRE(last_priority == 10);  // High priority wins

    printf("  PASS: stage_registry_priority\n");
}

static void test_stage_registry_fallback() {
    StageRegistry reg;

    // No claims — should return nullptr (fallback to generic)
    auto handler = reg.get_handler("skyrimse", "deploy");
    REQUIRE(!handler);

    // Claim for a different game shouldn't affect skyrimse
    reg.register_claim("isaac", "deploy",
        [](Mod&, PipelineContext&) -> bool { return true; },
        0, "isaac_plugin");

    handler = reg.get_handler("skyrimse", "deploy");
    REQUIRE(!handler);

    printf("  PASS: stage_registry_fallback\n");
}

static void test_hook_registry_basic() {
    HookRegistry reg;

    // No hooks yet
    REQUIRE(!reg.has_hooks("skyrimse.resolve.post"));

    int call_count = 0;

    // Register a hook
    reg.register_hook("skyrimse.resolve.post",
        [&](Mod&, PipelineContext&) { call_count++; },
        0, "plugin_a");

    REQUIRE(reg.has_hooks("skyrimse.resolve.post"));
    REQUIRE(!reg.has_hooks("skyrimse.launch.pre"));

    // Fire it
    Mod mod;
    PipelineContext ctx;
    reg.fire("skyrimse.resolve.post", mod, ctx);
    REQUIRE(call_count == 1);

    printf("  PASS: hook_registry_basic\n");
}

static void test_hook_registry_multiple() {
    HookRegistry reg;

    std::string call_order;

    reg.register_hook("skyrimse.deploy.pre",
        [&](Mod&, PipelineContext&) { call_order += "A"; },
        0, "plugin_a");

    reg.register_hook("skyrimse.deploy.pre",
        [&](Mod&, PipelineContext&) { call_order += "B"; },
        0, "plugin_b");

    reg.register_hook("skyrimse.deploy.pre",
        [&](Mod&, PipelineContext&) { call_order += "C"; },
        0, "plugin_c");

    Mod mod;
    PipelineContext ctx;
    reg.fire("skyrimse.deploy.pre", mod, ctx);

    // All three should fire in registration order (same priority = stable order)
    REQUIRE(call_order == "ABC");

    printf("  PASS: hook_registry_multiple\n");
}

static void test_hook_registry_priority_ordering() {
    HookRegistry reg;

    std::string call_order;

    // Register with different priorities — highest first
    reg.register_hook("test.tag",
        [&](Mod&, PipelineContext&) { call_order += "low"; },
        1, "low_priority");

    reg.register_hook("test.tag",
        [&](Mod&, PipelineContext&) { call_order += "high"; },
        10, "high_priority");

    reg.register_hook("test.tag",
        [&](Mod&, PipelineContext&) { call_order += "mid"; },
        5, "mid_priority");

    Mod mod;
    PipelineContext ctx;
    reg.fire("test.tag", mod, ctx);

    REQUIRE(call_order == "highmidlow");

    printf("  PASS: hook_registry_priority_ordering\n");
}

static void test_hook_registry_isolation() {
    HookRegistry reg;

    int tag_a_count = 0;
    int tag_b_count = 0;

    reg.register_hook("tag.a", [&](Mod&, PipelineContext&) { tag_a_count++; });
    reg.register_hook("tag.b", [&](Mod&, PipelineContext&) { tag_b_count++; });
    reg.register_hook("tag.a", [&](Mod&, PipelineContext&) { tag_a_count++; });

    Mod mod;
    PipelineContext ctx;

    reg.fire("tag.a", mod, ctx);
    REQUIRE(tag_a_count == 2);
    REQUIRE(tag_b_count == 0);

    reg.fire("tag.b", mod, ctx);
    REQUIRE(tag_a_count == 2);
    REQUIRE(tag_b_count == 1);

    printf("  PASS: hook_registry_isolation\n");
}

TEST_CASE("registry", "[engine]") {
    printf("Running registry tests...\n");

    test_stage_registry_basic();
    test_stage_registry_priority();
    test_stage_registry_fallback();
    test_hook_registry_basic();
    test_hook_registry_multiple();
    test_hook_registry_priority_ordering();
    test_hook_registry_isolation();
}
