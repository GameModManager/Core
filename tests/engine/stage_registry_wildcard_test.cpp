// StageRegistry wildcard stage claim resolution — exercises the exact-match
// beats-wildcard-at-equal-priority rule, higher-priority wildcard beats
// lower-priority exact-match, multiple wildcards (highest wins), has_claim
// semantics, and empty stage_name / game_id edge cases.
//
// Pure engine test: no plugin loading, no Qt, no UI.

#include "engine/pipeline/registry/stage_registry.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"

#include <cstdio>
#include <string>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

// A stage handler that sets a flag so we can tell it ran.
struct CallTracker {
    bool called = false;
    std::string game_seen;
    std::string stage_seen;

    StageFn make_handler(const std::string& gid, const std::string& stage) {
        return [this, gid, stage](Mod&, PipelineContext&) -> bool {
            called = true;
            game_seen = gid;
            stage_seen = stage;
            return true;
        };
    }
};

TEST_CASE("stage registry wildcard", "[engine]") {
    SECTION("wildcard claim applies to any game") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("", "Fomod", t.make_handler("", "Fomod"), 10, "wildcard_plugin");

        auto handler = reg.get_handler("SkyrimSpecialEdition", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(t.called);
        REQUIRE(t.game_seen == "");
    }

    SECTION("exact-match beats wildcard at equal priority") {
        StageRegistry reg;
        CallTracker wild, exact;
        reg.register_claim("", "Fomod", wild.make_handler("", "Fomod"), 10, "wild");
        reg.register_claim("SkyrimSE", "Fomod", exact.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        auto handler = reg.get_handler("SkyrimSE", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(exact.called);
        REQUIRE(!wild.called);
    }

    SECTION("higher-priority wildcard beats lower exact-match") {
        StageRegistry reg;
        CallTracker wild, exact;
        reg.register_claim("", "Fomod", wild.make_handler("", "Fomod"), 20, "wild");
        reg.register_claim("SkyrimSE", "Fomod", exact.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        auto handler = reg.get_handler("SkyrimSE", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(wild.called);
        REQUIRE(!exact.called);
    }

    SECTION("exact-match unaffected by wildcard for other games") {
        StageRegistry reg;
        CallTracker wild, exact;
        reg.register_claim("", "Fomod", wild.make_handler("", "Fomod"), 10, "wild");
        reg.register_claim("SkyrimSE", "Fomod", exact.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        // Game without an exact claim falls back to wildcard
        auto handler = reg.get_handler("Isaac", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(wild.called);
        REQUIRE(!exact.called);
    }

    SECTION("multiple wildcards — highest priority wins") {
        StageRegistry reg;
        CallTracker low, high;
        reg.register_claim("", "Fomod", low.make_handler("", "Fomod"), 5, "low");
        reg.register_claim("", "Fomod", high.make_handler("", "Fomod"), 15, "high");

        auto handler = reg.get_handler("AnyGame", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(high.called);
        REQUIRE(!low.called);
    }

    SECTION("multiple exact-match — highest priority wins") {
        StageRegistry reg;
        CallTracker low, high;
        reg.register_claim("SkyrimSE", "Fomod", low.make_handler("SkyrimSE", "Fomod"), 5, "low");
        reg.register_claim("SkyrimSE", "Fomod", high.make_handler("SkyrimSE", "Fomod"), 15, "high");

        auto handler = reg.get_handler("SkyrimSE", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(high.called);
        REQUIRE(!low.called);
    }

    SECTION("no claim — returns nullptr") {
        StageRegistry reg;
        REQUIRE(reg.get_handler("SkyrimSE", "Fomod") == nullptr);
    }

    SECTION("claim for different stage does not leak") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("", "Fomod", t.make_handler("", "Fomod"), 10, "wild");

        REQUIRE(reg.get_handler("SkyrimSE", "Deploy") == nullptr);
    }

    SECTION("has_claim — wildcard matches all games") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("", "Fomod", t.make_handler("", "Fomod"), 10, "wild");

        REQUIRE(reg.has_claim("SkyrimSE", "Fomod"));
        REQUIRE(reg.has_claim("Isaac", "Fomod"));
        REQUIRE(!reg.has_claim("SkyrimSE", "Deploy"));
    }

    SECTION("has_claim — exact matches only its game") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("SkyrimSE", "Fomod", t.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        REQUIRE(reg.has_claim("SkyrimSE", "Fomod"));
        REQUIRE(!reg.has_claim("Isaac", "Fomod"));
    }

    SECTION("has_claim — exact and wildcard both present") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("", "Fomod", t.make_handler("", "Fomod"), 10, "wild");
        reg.register_claim("SkyrimSE", "Fomod", t.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        REQUIRE(reg.has_claim("SkyrimSE", "Fomod"));
        REQUIRE(reg.has_claim("Isaac", "Fomod"));
    }

    SECTION("claims vector contains all registered claims") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("", "Fomod", t.make_handler("", "Fomod"), 10, "wild");
        reg.register_claim("SkyrimSE", "Fomod", t.make_handler("SkyrimSE", "Fomod"), 20, "exact");

        const auto& claims = reg.claims();
        REQUIRE(claims.size() == 2);
        REQUIRE(claims[0].game_id == "");
        REQUIRE(claims[0].priority == 10);
        REQUIRE(claims[1].game_id == "SkyrimSE");
        REQUIRE(claims[1].priority == 20);
    }

    SECTION("clear removes all claims") {
        StageRegistry reg;
        CallTracker t;
        reg.register_claim("", "Fomod", t.make_handler("", "Fomod"), 10, "wild");
        reg.register_claim("SkyrimSE", "Fomod", t.make_handler("SkyrimSE", "Fomod"), 20, "exact");

        reg.clear();
        REQUIRE(reg.claims().empty());
        REQUIRE(!reg.has_claim("SkyrimSE", "Fomod"));
        REQUIRE(reg.get_handler("SkyrimSE", "Fomod") == nullptr);
    }

    SECTION("game-specific beats higher-priority wildcard (p10 vs p10)") {
        // This tests the documented rule: "exact-match claims beat wildcard
        // claims at equal priority"
        StageRegistry reg;
        CallTracker wild, exact;
        reg.register_claim("", "Fomod", wild.make_handler("", "Fomod"), 10, "wild");
        reg.register_claim("SkyrimSE", "Fomod", exact.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        auto handler = reg.get_handler("SkyrimSE", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(exact.called);
        REQUIRE(!wild.called);
    }

    SECTION("wildcard beats exact when priority is strictly higher (p11 vs p10)") {
        StageRegistry reg;
        CallTracker wild, exact;
        reg.register_claim("", "Fomod", wild.make_handler("", "Fomod"), 11, "wild");
        reg.register_claim("SkyrimSE", "Fomod", exact.make_handler("SkyrimSE", "Fomod"), 10, "exact");

        auto handler = reg.get_handler("SkyrimSE", "Fomod");
        REQUIRE(handler != nullptr);
        Mod mod;
        PipelineContext ctx;
        handler(mod, ctx);
        REQUIRE(wild.called);
        REQUIRE(!exact.called);
    }

    SECTION("different stages are independent") {
        StageRegistry reg;
        CallTracker fomod, deploy;
        reg.register_claim("", "Fomod", fomod.make_handler("", "Fomod"), 10, "fomod_wild");
        reg.register_claim("", "Deploy", deploy.make_handler("", "Deploy"), 5, "deploy_wild");

        auto h1 = reg.get_handler("SkyrimSE", "Fomod");
        auto h2 = reg.get_handler("SkyrimSE", "Deploy");
        REQUIRE(h1 != nullptr);
        REQUIRE(h2 != nullptr);

        Mod mod;
        PipelineContext ctx;
        h1(mod, ctx);
        REQUIRE(fomod.called);
        REQUIRE(!deploy.called);
    }
}
