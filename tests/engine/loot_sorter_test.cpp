// Hermetic engine Sorter::Loot tests (PLAN.md §7.1, Phase 5.5): a fake
// gmm_lootcli shell script stands in for the real binary, so nothing here
// needs libloot/cargo. Covers: winning-path request building, stdout protocol
// parsing, progress streaming, sorted-output reading, CLI failure surfacing,
// and the missing-binary error.

#include "engine/sort/sorter/loot/masterlists.h"
#include "engine/sort/sorter/loot/sorter.h"
#include "platform/platform.h"

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

#define require(cond, msg)                                                             \
  do {                                                                                 \
    INFO(msg);                                                                         \
    REQUIRE((cond));                                                                   \
  } while (0)

namespace {

class FakePlatform : public engine::Platform {
public:
  explicit FakePlatform(fs::path data_dir) : data_dir_(std::move(data_dir)) {}
  std::string platform_name() const override { return "fake"; }
  fs::path data_dir() const override { return data_dir_; }
  fs::path config_dir() const override { return data_dir_; }
  fs::path cache_dir() const override { return data_dir_; }
  fs::path home_dir() const override { return data_dir_; }
  fs::path temp_dir() const override { return data_dir_; }
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
      // MO2 lootcli createPlugins shape: one findings-rich entry, one
      // name-only entry (skipped, like MO2), one plugin absent entirely.
      << "cat > \"$report\" <<'LOOTJSON'\n"
      << "{\n"
      << "  \"game\": \"skyrimse\",\n"
      << "  \"sortedPlugins\": [\"XPMSE.esp\", \"RaceMenu.esp\", \"SkyUI_SE.esp\"],\n"
      << "  \"plugins\": [\n"
      << "    {\"name\": \"XPMSE.esp\",\n"
      << "     \"incompatibilities\": [{\"name\": \"RaceMenu.esp\", "
         "\"displayName\": \"RaceMenu\"}],\n"
      << "     \"messages\": [{\"type\": \"warn\", \"text\": \"A <b>warning</b> "
         "with <a href=\\\"http://example.com\\\">link</a>\"},\n"
      << "                    {\"type\": \"error\", \"text\": \"Broken stuff\"},\n"
      << "                    {\"type\": \"info\", \"text\": \"Just saying\"}],\n"
      << "     \"dirty\": [{\"crc\": 12345, \"itm\": 3, \"deletedReferences\": 1, "
         "\"deletedNavmesh\": 2,\n"
      << "                \"cleaningUtility\": \"SSEEdit\", \"info\": \"extra "
         "info\"}],\n"
      << "     \"clean\": [{\"cleaningUtility\": \"SSEEdit\"}],\n"
      << "     \"missingMasters\": [\"GoneMaster.esm\"]},\n"
      << "    {\"name\": \"RaceMenu.esp\"}\n"
      << "  ]\n"
      << "}\n"
      << "LOOTJSON\n"
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

  engine::Sorter::Loot::Request request;
  request.game_id            = "SkyrimSpecialEdition";
  request.loot_game_id       = "skyrimse";
  request.masterlist_repo    = "skyrimse";
  request.game_dir           = base / "game";
  request.profile_dir        = base / "profile";
  request.cli_path           = cli;
  request.platform           = new FakePlatform(base / "data");
  request.update_masterlists = false;
  request.plugins            = {
      {"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"},
      {"RaceMenu.esp", "/fake/path/RaceMenu.esp"},
      {"XPMSE.esp", "/fake/path/XPMSE.esp"},
  };

  std::vector<int> stages;
  const engine::Sorter::Loot::Result result =
      engine::Sorter::Loot::run_sort(request, [&stages](int stage, const std::string&) {
        stages.push_back(stage);
      });

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
    if (stages[static_cast<size_t>(i)] != i + 1)
      in_order = false;
  require(in_order, "progress stages in order");

  // [level] messages relayed.
  bool have_info = false;
  for (const auto& m : result.messages)
    if (m.find("[info] fake masterlist loaded") != std::string::npos)
      have_info = true;
  require(have_info, "info message relayed");

  // The winning paths made it into the request file the CLI consumed.
  {
    std::ifstream in(base / "profile" / ".gmm_loot_tmp" / "loot_plugin_paths.txt");
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    require(content.find("/fake/path/SkyUI_SE.esp") != std::string::npos,
            "winning path passed to CLI");
    require(content.find("/fake/path/XPMSE.esp") != std::string::npos,
            "second winning path passed to CLI");
  }

  // Per-plugin LOOT findings parsed into Result::reports (MO2
  // LootDialog::showReport parity): XPMSE carries incompatibilities,
  // warn/error/info messages (HTML preserved raw), dirty + clean entries
  // and missing masters; the name-only RaceMenu entry and the absent
  // SkyUI entry yield no report.
  {
    require(result.reports.size() == 1, "one plugin report parsed");
    const auto it = result.reports.find("XPMSE.esp");
    require(it != result.reports.end(), "XPMSE report present");
    const engine::LootReport& rep = it->second;
    require(rep.incompatibilities.size() == 1, "one incompatibility");
    require(rep.incompatibilities[0].first == "RaceMenu.esp", "incompatibility name");
    require(rep.incompatibilities[0].second == "RaceMenu",
            "incompatibility display name");
    require(rep.messages.size() == 3, "three LOOT messages");
    require(rep.messages[0].level == "warning", "warn maps to warning");
    require(rep.messages[0].text.find("<a href=\"http://example.com\">") !=
                std::string::npos,
            "message HTML preserved raw");
    require(rep.messages[1].level == "error", "error level kept");
    require(rep.messages[1].text == "Broken stuff", "error text");
    require(rep.messages[2].level == "info", "info level kept");
    require(rep.dirty.size() == 1, "one dirty entry");
    require(rep.dirty[0].itm_records == 3, "dirty ITM count");
    require(rep.dirty[0].deleted_references == 1, "dirty deleted references");
    require(rep.dirty[0].deleted_navmeshes == 2, "dirty deleted navmeshes");
    require(rep.dirty[0].cleaning_utility == "SSEEdit", "dirty utility");
    require(rep.dirty[0].info == "extra info", "dirty info");
    require(rep.clean.size() == 1, "one clean entry");
    require(rep.clean[0].cleaning_utility == "SSEEdit", "clean utility");
    require(rep.missing_masters.size() == 1 &&
                rep.missing_masters[0] == "GoneMaster.esm",
            "LOOT missing masters");
    require(result.reports.find("RaceMenu.esp") == result.reports.end(),
            "name-only entry yields no report");
    require(result.reports.find("SkyUI_SE.esp") == result.reports.end(),
            "absent plugin yields no report");
  }

  delete request.platform;
  std::fprintf(stderr, "loot_sorter_test: success case OK\n");
}

void run_failure_case(const fs::path& base) {
  const fs::path cli_dir = base / "cli_fail";
  fs::create_directories(cli_dir);
  const fs::path cli = make_fake_cli(cli_dir, 3);

  engine::Sorter::Loot::Request request;
  request.game_id            = "SkyrimSpecialEdition";
  request.loot_game_id       = "skyrimse";
  request.masterlist_repo    = "skyrimse";
  request.game_dir           = base / "game2";
  request.profile_dir        = base / "profile2";
  request.cli_path           = cli;
  request.platform           = new FakePlatform(base / "data2");
  request.update_masterlists = false;
  request.plugins            = {{"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"}};

  const engine::Sorter::Loot::Result result = engine::Sorter::Loot::run_sort(request);

  require(!result.ok, "CLI failure surfaces");
  require(result.sorted_names.empty(), "no sorted names on failure");
  require(result.error.find("fake failure") != std::string::npos,
          "CLI stderr surfaced in error");

  delete request.platform;
  std::fprintf(stderr, "loot_sorter_test: failure case OK\n");
}

void run_missing_cli_case(const fs::path& base) {
  engine::Sorter::Loot::Request request;
  request.game_id            = "SkyrimSpecialEdition";
  request.loot_game_id       = "skyrimse";
  request.masterlist_repo    = "skyrimse";
  request.game_dir           = base / "game3";
  request.profile_dir        = base / "profile3";
  request.cli_path           = base / "does_not_exist";
  request.platform           = new FakePlatform(base / "data3");
  request.update_masterlists = false;
  request.plugins            = {{"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"}};

  const engine::Sorter::Loot::Result result = engine::Sorter::Loot::run_sort(request);

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

  engine::Sorter::Loot::Request request;
  request.game_id         = "SkyrimSpecialEdition";
  request.loot_game_id    = "skyrimse";
  request.masterlist_repo = "skyrimse";
  request.game_dir        = base / "game4";
  request.profile_dir     = base / "profile4";
  request.cli_path        = cli;
  request.platform        = nullptr;
  request.plugins         = {{"SkyUI_SE.esp", "/fake/path/SkyUI_SE.esp"}};

  const engine::Sorter::Loot::Result result = engine::Sorter::Loot::run_sort(request);

  require(!result.ok, "no platform -> masterlists unavailable");
  require(!result.error.empty(), "error explains the failure");

  std::fprintf(stderr, "loot_sorter_test: masterlist-fallback case OK\n");
}

// Regression test for the gmm_lootcli emitter bug that wrote UNCLOSED plugin
// objects (each entry missed its '}'), which made every real loot_report.json
// unparsable so no LOOT bullets ever reached the DB. The golden file mirrors
// tools/gmm_lootcli/src/main.cpp write_plugin_reports() output style
// (single-line objects, emitter field order, trailing flag-only entries); if
// the emitter regresses, the parse below yields no reports and this fails.
void run_golden_report_case(const fs::path& base) {
  const fs::path golden =
      fs::path(__FILE__).parent_path() / "fixtures" / "loot_report_golden.json";
  require(fs::is_regular_file(golden), "golden report fixture exists");

  const fs::path cli_dir = base / "cli_golden";
  fs::create_directories(cli_dir);
  std::ostringstream s;
  s << "#!/bin/sh\n"
    << "echo '[progress] 7'\n"
    << "out=\"\"\nreport=\"\"\nprev=\"\"\n"
    << "for arg in \"$@\"; do\n"
    << "  if [ \"$prev\" = \"--pluginListOutputPath\" ]; then out=\"$arg\"; fi\n"
    << "  if [ \"$prev\" = \"--out\" ]; then report=\"$arg\"; fi\n"
    << "  prev=\"$arg\"\n"
    << "done\n"
    << "printf 'Skyrim.esm\\nUpdate.esm\\nDawnguard.esm\\nTrueHUD.esl\\n"
       "RaceMenu.esp\\nRaceMenuPlugin.esp\\nXPMSE.esp\\n' > \"$out\"\n"
    << "cat \"" << golden.string() << "\" > \"$report\"\n"
    << "exit 0\n";
  const fs::path cli = cli_dir / "fake_gmm_lootcli_golden";
  write_file(cli, s.str());
  chmod(cli.c_str(), 0755);

  engine::Sorter::Loot::Request request;
  request.game_id            = "SkyrimSpecialEdition";
  request.loot_game_id       = "skyrimse";
  request.masterlist_repo    = "skyrimse";
  request.game_dir           = base / "game_golden";
  request.profile_dir        = base / "profile_golden";
  request.cli_path           = cli;
  request.platform           = new FakePlatform(base / "data_golden");
  request.update_masterlists = false;
  request.plugins            = {
      {"Skyrim.esm", "/fake/path/Skyrim.esm"},
      {"Update.esm", "/fake/path/Update.esm"},
      {"Dawnguard.esm", "/fake/path/Dawnguard.esm"},
      {"TrueHUD.esl", "/fake/path/TrueHUD.esl"},
      {"RaceMenu.esp", "/fake/path/RaceMenu.esp"},
      {"RaceMenuPlugin.esp", "/fake/path/RaceMenuPlugin.esp"},
      {"XPMSE.esp", "/fake/path/XPMSE.esp"},
  };

  const engine::Sorter::Loot::Result result = engine::Sorter::Loot::run_sort(request);
  require(result.ok, "golden sort succeeds");
  require(result.sorted_names.size() == 7, "7 golden sorted names");

  // Flags-only entries (Skyrim.esm, TrueHUD.esl) and the name-only entry
  // (XPMSE.esp) carry no tooltip data, so only 4 reports survive parsing.
  require(result.reports.size() == 4, "4 golden plugin reports parsed");

  {
    const auto it = result.reports.find("Update.esm");
    require(it != result.reports.end(), "Update.esm report present");
    require(it->second.dirty.size() == 1, "Update.esm dirty entry");
    require(it->second.dirty[0].itm_records == 386, "dirty ITM count");
    require(it->second.dirty[0].deleted_references == 93, "dirty references");
    require(it->second.dirty[0].deleted_navmeshes == 3, "dirty navmeshes");
    require(it->second.dirty[0].cleaning_utility.find("SSEEdit") != std::string::npos,
            "dirty utility");
    require(it->second.dirty[0].info.find("xEdit") != std::string::npos, "dirty info");
  }

  {
    const auto it = result.reports.find("Dawnguard.esm");
    require(it != result.reports.end(), "Dawnguard.esm report present");
    const engine::LootReport& rep = it->second;
    require(rep.incompatibilities.size() == 2, "two incompatibilities");
    require(rep.incompatibilities[0].first == "XPMSE.esp", "incompat name");
    require(rep.incompatibilities[0].second == "XP32 Maximum Skeleton",
            "incompat display name");
    require(rep.incompatibilities[1].first == "RaceMenu.esp", "2nd incompat");
    require(rep.incompatibilities[1].second.empty(), "2nd display empty");
    require(rep.messages.size() == 2, "two LOOT messages");
    require(rep.messages[0].level == "warning", "warn maps to warning");
    require(rep.messages[1].level == "error", "error level kept");
    require(rep.missing_masters.size() == 1 &&
                rep.missing_masters[0] == "GoneMaster.esm",
            "LOOT missing masters");
  }

  {
    const auto it = result.reports.find("RaceMenu.esp");
    require(it != result.reports.end(), "RaceMenu.esp report present");
    require(it->second.clean.size() == 1, "RaceMenu.esp clean entry");
    require(it->second.clean[0].cleaning_utility == "SSEEdit v4.0.3", "clean utility");
    const auto pit = result.reports.find("RaceMenuPlugin.esp");
    require(pit != result.reports.end(), "RaceMenuPlugin.esp report present");
    require(pit->second.messages.size() == 1, "optional-plugin message");
    require(pit->second.messages[0].level == "info", "info level kept");
    require(pit->second.clean.size() == 1, "plugin clean entry");
  }

  require(result.reports.find("Skyrim.esm") == result.reports.end(),
          "flags-only entry yields no report");
  require(result.reports.find("TrueHUD.esl") == result.reports.end(),
          "light-flag entry yields no report");
  require(result.reports.find("XPMSE.esp") == result.reports.end(),
          "name-only entry yields no report");

  delete request.platform;
  std::fprintf(stderr, "loot_sorter_test: golden case OK\n");
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

TEST_CASE("loot sorter golden emitter report", "[engine]") {
  const fs::path base = "/tmp/gmm_loot_golden_test";
  std::error_code ec;
  fs::remove_all(base, ec);

  // Pre-seed a fresh masterlist so the manager uses the cache (no network).
  const fs::path loot = base / "data_golden" / "loot" / "skyrimse";
  fs::create_directories(loot, ec);
  write_file(loot / "masterlist.yaml", "masterlist: 1\n");
  write_file(loot / "prelude.yaml", "prelude: 1\n");

  run_golden_report_case(base);
}
