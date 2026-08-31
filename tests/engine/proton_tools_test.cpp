// Engine test for the Proton tool-running module (proton_tools).
//
// Covers runner resolution (override wins, display-name lookup via the
// platform, fallback to per-game/latest when unresolvable, null platform) and
// the guard cases of run_proton_exe. The exec paths themselves are thin
// fork+execvp orchestrations over platform discovery — not unit-tested here.
#include "engine/deploy/launch/proton_tools.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
}
}

// --- Stub platform: controllable proton discovery ---
class StubPlatform : public engine::Platform {
public:
    std::string platform_name() const override { return "test"; }
    fs::path data_dir() const override { return "/tmp/gmm_proton_test_data"; }
    fs::path config_dir() const override { return "/tmp/gmm_proton_test_config"; }
    fs::path cache_dir() const override { return "/tmp/gmm_proton_test_cache"; }
    fs::path home_dir() const override { return data_dir(); }
    fs::path temp_dir() const override { return data_dir(); }
    fs::path find_steam_root() const override { return {}; }
    bool launch_executable(const fs::path&,
                           const std::vector<std::string>&) const override {
        return false;
    }

    fs::path named_result;    // returned by find_proton_named
    fs::path default_result;  // returned by find_proton / find_proton_for_game

    fs::path find_proton() const override { return default_result; }
    fs::path find_proton_for_game(uint32_t) const override { return default_result; }
    fs::path find_proton_named(const std::string&) const override {
        return named_result;
    }
};

TEST_CASE("proton tools", "[engine]") {
    StubPlatform platform;

    // --- resolve_proton_runner ---
    // 1. Empty override -> platform default (latest/per-game).
    {
        engine::ProtonToolRequest req;
        req.platform = &platform;
        platform.default_result = "/steam/proton";
        require(engine::resolve_proton_runner(req) == fs::path("/steam/proton"),
                "empty override resolves to platform default");
    }

    // 2. Override resolvable through the platform -> override wins.
    {
        engine::ProtonToolRequest req;
        req.platform = &platform;
        platform.default_result = "/steam/proton";
        platform.named_result = "/steam/Proton 10.0/proton";
        req.runner_override = "Proton 10.0";
        require(engine::resolve_proton_runner(req) ==
                    fs::path("/steam/Proton 10.0/proton"),
                "resolvable display-name override wins");
    }

    // 3. Override the platform cannot resolve -> falls back to default.
    {
        engine::ProtonToolRequest req;
        req.platform = &platform;
        platform.default_result = "/steam/proton";
        platform.named_result.clear();
        req.runner_override = "Proton Bogus";
        require(engine::resolve_proton_runner(req) == fs::path("/steam/proton"),
                "unresolvable override falls back to default");
    }

    // 4. No platform -> no runner.
    {
        engine::ProtonToolRequest req;
        req.runner_override = "Proton 10.0";
        require(engine::resolve_proton_runner(req).empty(),
                "null platform resolves no runner");
    }

    // --- run_proton_exe guards ---
    // 5. Empty path.
    {
        engine::ProtonToolRequest req;
        req.platform = &platform;
        require(engine::run_proton_exe(req, fs::path()) == -1,
                "empty exe path refused");
    }

    // 6. Nonexistent exe.
    {
        engine::ProtonToolRequest req;
        req.platform = &platform;
        require(engine::run_proton_exe(req, "/definitely/not/here.exe") == -1,
                "nonexistent exe refused");
    }

    // 7. Real exe + resolvable runner -> detached spawn succeeds (PID > 0).
    //    The fake `proton` script exits immediately; we only assert the fork.
    {
        auto tmp = fs::temp_directory_path();
        auto exe_path = tmp / "gmm_proton_tools_test.exe";
        auto proton_path = tmp / "gmm_proton_tools_test_proton";
        {
            std::ofstream out(exe_path);
            out << "fake exe contents";
        }
        {
            std::ofstream out(proton_path);
            out << "#!/bin/sh\nexit 0\n";
        }
        std::error_code ec;
        fs::permissions(proton_path, fs::perms::owner_all, ec);

        engine::ProtonToolRequest req;
        req.platform = &platform;
        req.game_dir = tmp;
        platform.named_result = proton_path;
        platform.default_result = proton_path;
        req.runner_override = "Fake";
        auto pid = engine::run_proton_exe(req, exe_path);
        require(pid > 0, "real exe spawns detached child");

        fs::remove(exe_path, ec);
        fs::remove(proton_path, ec);
    }
}
