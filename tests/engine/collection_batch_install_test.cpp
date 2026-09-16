// Tests for the source-agnostic batch install driver.
//
// Covers:
//   - expected_hash: per-source extraction, prefix stripping, empty/steam
//   - phase order respected, manifest order within a phase
//   - up-to-date skip on hash match, install on mismatch
//   - unresolvable entries skipped without aborting the batch
//   - per-mod failures don't abort the batch
//   - user cancel stops the batch, rest reported NotAttempted
//   - pipeline gets a copy (collection stays pristine)

#include "engine/collection/batch_installer.h"
#include "engine/collection/resolver.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace engine::Collection;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static ModEntry make_nexus_entry(const std::string& id, int phase,
                                 const std::string& sha = "abc123") {
    ModEntry e;
    e.id = id;
    e.name = id + "-name";
    e.phase = phase;
    SourceNexus s;
    s.game_domain = "skyrimspecialedition";
    s.mod_id = 42;
    s.file_id = 7;
    s.sha256 = sha;
    e.source = s;
    return e;
}

static Manifest make_manifest(std::vector<ModEntry> entries) {
    Manifest m;
    m.schema_version = "1.0.0";
    m.id = "test-uuid";
    m.revision = 1;
    m.info.name = "Test Collection";
    m.mods = std::move(entries);
    return m;
}

// ---------------------------------------------------------------------------
// expected_hash
// ---------------------------------------------------------------------------

TEST_CASE("expected_hash extracts nexus sha", "[collection][batch]")
{
    SourceNexus s;
    s.sha256 = "deadbeef";
    REQUIRE(expected_hash(ModSource{s}) == std::optional<std::string>{"deadbeef"});
}

TEST_CASE("expected_hash strips sha256 prefix", "[collection][batch]")
{
    SourceDirect s;
    s.sha256 = "sha256:deadbeef";
    REQUIRE(expected_hash(ModSource{s}) == std::optional<std::string>{"deadbeef"});
}

TEST_CASE("expected_hash empty when no hash", "[collection][batch]")
{
    SourceNexus s; // sha256 empty
    REQUIRE_FALSE(expected_hash(ModSource{s}).has_value());

    SourceSteamWorkshop w;
    w.workshop_item_id = 123;
    REQUIRE_FALSE(expected_hash(ModSource{w}).has_value());
}

// ---------------------------------------------------------------------------
// install: empty + success + order
// ---------------------------------------------------------------------------

TEST_CASE("batch install empty collection", "[collection][batch]")
{
    int calls = 0;
    BatchInstallerDeps deps;
    deps.install_one = [&](::engine::Mod&) {
        ++calls;
        return engine::PipelineResult::Success;
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({});
    ResolvedCollection rc = Resolver{}.resolve(m);
    BatchResult r = bi.install(rc);

    REQUIRE(r.mods.empty());
    REQUIRE(r.installed == 0);
    REQUIRE(calls == 0);
    REQUIRE_FALSE(r.canceled);
}

TEST_CASE("batch install respects phases and manifest order", "[collection][batch]")
{
    std::vector<std::string> order;
    BatchInstallerDeps deps;
    deps.install_one = [&](::engine::Mod& mod) {
        order.push_back(mod.id);
        return engine::PipelineResult::Success;
    };
    BatchInstaller bi(std::move(deps));

    // Manifest order scrambled across phases; phases must win, manifest
    // order breaks ties within a phase.
    Manifest m = make_manifest({
        make_nexus_entry("late-b", 2),
        make_nexus_entry("early-a", 0),
        make_nexus_entry("mid-c", 1),
        make_nexus_entry("early-d", 0),
        make_nexus_entry("late-e", 2),
    });
    ResolvedCollection rc = Resolver{}.resolve(m);
    BatchResult r = bi.install(rc);

    REQUIRE(r.installed == 5);
    REQUIRE(order ==
            std::vector<std::string>{"early-a", "early-d", "mid-c", "late-b", "late-e"});
    REQUIRE(r.mods.size() == 5);
    for (const auto& mr : r.mods)
        REQUIRE(mr.verdict == ModVerdict::Installed);
}

// ---------------------------------------------------------------------------
// install: up-to-date skip
// ---------------------------------------------------------------------------

TEST_CASE("batch skips up-to-date mods on hash match", "[collection][batch]")
{
    std::vector<std::string> installed_ids;
    BatchInstallerDeps deps;
    deps.install_one = [&](::engine::Mod& mod) {
        installed_ids.push_back(mod.id);
        return engine::PipelineResult::Success;
    };
    deps.installed_hash_for = [](const std::string& id) -> std::optional<std::string> {
        if (id == "current")
            return std::optional<std::string>{"abc123"};
        if (id == "prefixed")
            return std::optional<std::string>{"sha256:abc123"};
        return std::nullopt; // not installed
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({
        make_nexus_entry("current", 0),
        make_nexus_entry("prefixed", 0),
        make_nexus_entry("stale", 0, "new-hash"),
        make_nexus_entry("fresh", 0),
    });
    ResolvedCollection rc = Resolver{}.resolve(m);
    // "fresh" has sha abc123 but no installed record -> must install.
    BatchResult r = bi.install(rc);

    REQUIRE(r.skipped_up_to_date == 2);
    REQUIRE(r.installed == 2);
    REQUIRE(installed_ids == std::vector<std::string>{"stale", "fresh"});
}

// ---------------------------------------------------------------------------
// install: failures don't abort, cancel stops
// ---------------------------------------------------------------------------

TEST_CASE("batch continues past per-mod failures", "[collection][batch]")
{
    BatchInstallerDeps deps;
    deps.install_one = [](::engine::Mod& mod) {
        if (mod.id == "broken")
            return engine::PipelineResult::Failed;
        return engine::PipelineResult::Success;
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({
        make_nexus_entry("good-a", 0),
        make_nexus_entry("broken", 0),
        make_nexus_entry("good-b", 1),
    });
    ResolvedCollection rc = Resolver{}.resolve(m);
    BatchResult r = bi.install(rc);

    REQUIRE(r.installed == 2);
    REQUIRE(r.failed == 1);
    REQUIRE_FALSE(r.canceled);
    REQUIRE(r.mods.size() == 3);
    REQUIRE(r.mods[1].verdict == ModVerdict::Failed);
    REQUIRE(r.mods[1].mod_id == "broken");
    REQUIRE_FALSE(r.mods[1].message.empty());
}

TEST_CASE("batch stops on user cancel", "[collection][batch]")
{
    std::vector<std::string> attempted;
    BatchInstallerDeps deps;
    deps.install_one = [&](::engine::Mod& mod) {
        attempted.push_back(mod.id);
        if (mod.id == "abort-me")
            return engine::PipelineResult::Canceled;
        return engine::PipelineResult::Success;
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({
        make_nexus_entry("first", 0),
        make_nexus_entry("abort-me", 0),
        make_nexus_entry("never-a", 1),
        make_nexus_entry("never-b", 1),
    });
    ResolvedCollection rc = Resolver{}.resolve(m);
    BatchResult r = bi.install(rc);

    REQUIRE(r.canceled);
    REQUIRE(r.installed == 1);
    REQUIRE(attempted == std::vector<std::string>{"first", "abort-me"});
    REQUIRE(r.mods.size() == 4);
    REQUIRE(r.mods[1].verdict == ModVerdict::Canceled);
    REQUIRE(r.mods[2].verdict == ModVerdict::NotAttempted);
    REQUIRE(r.mods[3].verdict == ModVerdict::NotAttempted);
}

// ---------------------------------------------------------------------------
// install: wiring edge cases + collection pristine
// ---------------------------------------------------------------------------

TEST_CASE("batch without installer fails every mod", "[collection][batch]")
{
    BatchInstaller bi(BatchInstallerDeps{}); // no install_one

    Manifest m = make_manifest({make_nexus_entry("a", 0)});
    ResolvedCollection rc = Resolver{}.resolve(m);
    BatchResult r = bi.install(rc);

    REQUIRE(r.failed == 1);
    REQUIRE(r.mods[0].verdict == ModVerdict::Failed);
}

TEST_CASE("batch progress reported per entry", "[collection][batch]")
{
    std::vector<std::string> seen;
    int last_done = 0;
    int last_total = 0;
    BatchInstallerDeps deps;
    deps.install_one = [](::engine::Mod&) {
        return engine::PipelineResult::Success;
    };
    deps.on_progress = [&](int done, int total, const std::string& id) {
        last_done = done;
        last_total = total;
        seen.push_back(id);
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({
        make_nexus_entry("a", 0),
        make_nexus_entry("b", 1),
    });
    ResolvedCollection rc = Resolver{}.resolve(m);
    (void)bi.install(rc);

    REQUIRE(last_done == 2);
    REQUIRE(last_total == 2);
    REQUIRE(seen == std::vector<std::string>{"a", "b"});
}

TEST_CASE("batch skips unresolvable entries without aborting", "[collection][batch]")
{
    int calls = 0;
    BatchInstallerDeps deps;
    deps.install_one = [&](::engine::Mod& mod) {
        ++calls;
        (void)mod;
        return engine::PipelineResult::Success;
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({
        make_nexus_entry("good-a", 0),
        make_nexus_entry("good-b", 0),
    });
    ResolvedCollection rc = Resolver{}.resolve(m);
    // Simulate an unknown source provider on the first entry.
    rc.mods[0].resolvable = false;
    rc.mods[0].error = "Unknown source provider: foo";
    BatchResult r = bi.install(rc);

    REQUIRE(r.installed == 1);
    REQUIRE(r.skipped_unresolvable == 1);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(r.canceled);
    REQUIRE(r.mods.size() == 2);
    REQUIRE(r.mods[0].verdict == ModVerdict::SkippedUnresolvable);
    REQUIRE(r.mods[1].verdict == ModVerdict::Installed);
}

TEST_CASE("batch pipeline gets a copy, collection stays pristine", "[collection][batch]")
{
    BatchInstallerDeps deps;
    deps.install_one = [](::engine::Mod& mod) {
        mod.name = "MUTATED";
        mod.state = ::engine::ModState::Deployed;
        return engine::PipelineResult::Success;
    };
    BatchInstaller bi(std::move(deps));

    Manifest m = make_manifest({make_nexus_entry("a", 0)});
    ResolvedCollection rc = Resolver{}.resolve(m);
    (void)bi.install(rc);

    REQUIRE(rc.mods[0].mod.name == "a-name");
    REQUIRE(rc.mods[0].mod.state == ::engine::ModState::Downloaded);
}
