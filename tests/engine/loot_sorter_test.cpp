// Hermetic engine LootSorter tests (PLAN.md §7.1, Phase 5.5): a fake
// gmm_lootcli shell script stands in for the real binary, so nothing here
// needs libloot/cargo. Covers: winning-path request building, stdout protocol
// parsing, progress streaming, sorted-output reading, CLI failure surfacing,
// and the missing-binary error.

#include "engine/sort/loot/loot_sorter.h"
#include "engine/sort/loot/masterlists.h"
#include "platform/platform_interface.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace fs = std::filesystem;

#define require(cond, msg)                                       \
    do {                                                         \
        INFO(msg);                                               \
        REQUIRE((cond));                                         \
    } while (0)

namespace {

class FakePlatform : public engine::PlatformInterface {
public:
    explicit FakePlatform(fs::path data_dir) : data_dir_(std::move(data_dir)) {}
    std::string platform_name() const override { return "fake"; }
    fs::path data_dir() const override { return data_dir_; }
    fs::path config_dir() const override { return data_dir_; }
    fs::path cache_dir() const override { return data_dir_; }
    fs::path find_steam_root() const override { return {}; }
    bool launch_executable(const fs::path&,
                           const std::vector<std::string>&) const override {
        return false;
    }

private:
    fs::path data_dir_;
};

void write_file(const fs::path& p, const std::string& content) {
    std::ofstream out(p);
    out << content;
}

std::string fake_cli_script(int exit_code) {
    std::ostringstream s;
    s << "#!/bin/sh\n"
      << "echo '[progress] 1'\n"
      << "echo '[progress] 2'\n"
      << "echo '[info] fake masterlist loaded'\n"
      << "echo '[progress] 3'\n"
      << "echo '[progress] 4'\n"
      << "echo '[progress] 5'\n"
      << "echo '[progress] 6'\n"
      << "out=\"\"\nreport=\"\"\npaths=\"\"\nprev=\"\"\n"
      << "for arg in \"$@\"; do\n"
      << "  if [ \"$prev\" = \"--pluginListOutputPath\" ]; then out=\"$arg\"; fi\n"
      << "  if [ \"$prev\" = \"--out\" ]; then report=\"$arg\"; fi\n"
      << "  if [ \"$prev\" = \"--pluginPathsFile\" ]; then paths=\"$arg\"; fi\n"
      << "  prev=\"$arg\"\n"
      << "done\n"
      << "echo '[progress] 7'\n";
    if (exit_code != 0) {
        s << "echo \"Error: fake failure\" >&2\n"
          << "exit " << exit_code << "\n";
    } else {
        s << "printf '# fake sorted\\n' > \"$out\"\n"
          << "printf 'XPMSE.esp\\n' >> \"$out\"\n"
          << "printf 'RaceMenu.esp\\n' >> \"$out\"\n"
          << "printf 'SkyUI_SE.esp\\n' >> \"$out\"\n"
          << "printf '{\"paths\":' > \"$report\"\n"
          << "cat \"$paths\" >> \"$report\"\n"
          << "printf '}\\n' >> \"$report\"\n"
          << "exit 0\n";
    }
    return s.str();
}

fs::path make_fake_cli(const fs::path& dir, int exit_code) {
    const fs::path cli = dir / "fake_gmm_lootcli";
    write_file(cli, fake_cli_script(exit_code));
    chmod(cli.c_str(), 0755);
    return cli;
}

void run_success_case(const fs::path& base) {
    const fs::path cli_dir = base / "cli";
    fs::create_directories(cli_dir);
    const fs::path cli = make_fake_cli(cli_dir, 0);

    engine::LootRequest request;
    request.game_id = "SkyrimSpecialEdition";
    request.loot_game_id = "skyrimse";
    request.masterlist_repo = "skyrimse";
    request.game_dir = base / "game";
    request.profile_dir = base / "profile";
    request.cli_path = cli;
    request.platform = new FakePlatform(base / "data");
    request.plugins = {
        {"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"},
        {"RaceMenu.esp", "/fake/path/RaceMenu.esp"},
        {"XPMSE.esp", "/fake/path/XPMSE.esp"},
    };

    std::vector<int> stages;
    const engine::LootResult result = engine::run_loot_sort(
        request, [&stages](int stage, const std::string&) { stages.push_back(stage); });

    require(result.ok, "successful sort");
    require(result.error.empty(), "no error on success");
    require(result.sorted_names.size() == 3, "3 sorted names");
    require(result.sorted_names[0] == "XPMSE.esp", "first sorted name");
    require(result.sorted_names[2] == "SkyUI_SE.esp", "last sorted name");
    require(!result.report_path.empty() && fs::is_regular_file(result.report_path),
            "report file written");

    // Progress stages 1..8 must have streamed (in order).
    require(stages.size() == 8, "8 progress stages");
    bool in_order = true;
    for (int i = 0; i < 8; ++i)
        if (stages[static_cast<size_t>(i)] != i + 1) in_order = false;
    require(in_order, "progress stages in order");

    // [level] messages relayed.
    bool have_info = false;
    for (const auto& m : result.messages)
        if (m.find("[info] fake masterlist loaded") != std::string::npos)
            have_info = true;
    require(have_info, "info message relayed");

    // The winning paths made it into the request file the CLI consumed.
    {
        std::ifstream in(result.report_path);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        require(content.find("/fake/path/SkyUI_SE.esp") != std::string::npos,
                "winning path passed to CLI");
        require(content.find("/fake/path/XPMSE.esp") != std::string::npos,
                "second winning path passed to CLI");
    }

    delete request.platform;
    std::fprintf(stderr, "loot_sorter_test: success case OK\n");
}

void run_failure_case(const fs::path& base) {
    const fs::path cli_dir = base / "cli_fail";
    fs::create_directories(cli_dir);
    const fs::path cli = make_fake_cli(cli_dir, 3);

    engine::LootRequest request;
    request.game_id = "SkyrimSpecialEdition";
    request.loot_game_id = "skyrimse";
    request.masterlist_repo = "skyrimse";
    request.game_dir = base / "game2";
    request.profile_dir = base / "profile2";
    request.cli_path = cli;
    request.platform = new FakePlatform(base / "data2");
    request.plugins = {{"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"}};

    const engine::LootResult result = engine::run_loot_sort(request);

    require(!result.ok, "CLI failure surfaces");
    require(result.sorted_names.empty(), "no sorted names on failure");
    require(result.error.find("fake failure") != std::string::npos,
            "CLI stderr surfaced in error");

    delete request.platform;
    std::fprintf(stderr, "loot_sorter_test: failure case OK\n");
}

void run_missing_cli_case(const fs::path& base) {
    engine::LootRequest request;
    request.game_id = "SkyrimSpecialEdition";
    request.loot_game_id = "skyrimse";
    request.masterlist_repo = "skyrimse";
    request.game_dir = base / "game3";
    request.profile_dir = base / "profile3";
    request.cli_path = base / "does_not_exist";
    request.platform = new FakePlatform(base / "data3");
    request.plugins = {{"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"}};

    const engine::LootResult result = engine::run_loot_sort(request);

    require(!result.ok, "missing CLI surfaces");
    require(result.error.find("gmm_lootcli") != std::string::npos,
            "error names gmm_lootcli");

    delete request.platform;
    std::fprintf(stderr, "loot_sorter_test: missing-CLI case OK\n");
}

void run_masterlist_fallback_case(const fs::path& base) {
    // A valid CLI but no platform data dir: masterlists cannot resolve, so the
    // sort must fail with a clear error before any subprocess runs.
    const fs::path cli_dir = base / "cli_fallback";
    fs::create_directories(cli_dir);
    const fs::path cli = make_fake_cli(cli_dir, 0);

    engine::LootRequest request;
    request.game_id = "SkyrimSpecialEdition";
    request.loot_game_id = "skyrimse";
    request.masterlist_repo = "skyrimse";
    request.game_dir = base / "game4";
    request.profile_dir = base / "profile4";
    request.cli_path = cli;
    request.platform = nullptr;
    request.plugins = {{"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"}};

    const engine::LootResult result = engine::run_loot_sort(request);

    require(!result.ok, "no platform -> masterlists unavailable");
    require(!result.error.empty(), "error explains the failure");

    std::fprintf(stderr, "loot_sorter_test: masterlist-fallback case OK\n");
}

}  // namespace

TEST_CASE("loot sorter", "[engine]") {
    const fs::path base = "/tmp/gmm_loot_sorter_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    // Pre-seed fresh masterlists so the manager uses the cache (no network).
    for (const char* sub : {"data", "data2", "data3"}) {
        const fs::path loot = base / sub / "loot" / "skyrimse";
        fs::create_directories(loot, ec);
        write_file(loot / "masterlist.yaml", "masterlist: 1\n");
        write_file(loot / "prelude.yaml", "prelude: 1\n");
    }

    run_success_case(base);
    run_failure_case(base);
    run_missing_cli_case(base);
    run_masterlist_fallback_case(base);
}
