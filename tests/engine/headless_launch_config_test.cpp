// Headless launch config helpers (Workspace-cch0): Qt-free engine surface
// the headless CLI uses to honor per-executable args/env/cwd.
//   - split_launch_arguments mirrors the GUI split_arguments tokenization
//     exactly (whitespace split, double-quote grouping, backslash escape).
//   - resolve_launch_cwd mirrors the GUI resolve_start_in.
//   - lookup_executable_launch_config reads the instance.toml executables
//     array and returns the first entry matching the launched binary
//     (game-relative, case-insensitive, first match wins).
// Hermetic: all fixtures live under the system temp dir; no Qt, no spawn.
#include "engine/core/instance/instance_utils.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

void write_file(const fs::path &p, const std::string &contents) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << contents;
  REQUIRE(out.good());
}

fs::path fresh_root(const std::string &name) {
  const fs::path root =
      fs::temp_directory_path() /
      ("gmm_test_headless_cfg_" + std::to_string(getpid()) + "_" + name);
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}
}  // namespace

TEST_CASE("split launch arguments", "[engine]") {
  using engine::split_launch_arguments;
  check(split_launch_arguments("").empty(), "empty string splits to no args");
  check(split_launch_arguments("   ").empty(), "whitespace only splits to no args");

  const auto simple = split_launch_arguments("-foo bar baz");
  check(simple == std::vector<std::string>{"-foo", "bar", "baz"},
        "whitespace-separated tokens split");

  const auto multi = split_launch_arguments("  -foo\t bar  \n baz ");
  check(multi == std::vector<std::string>{"-foo", "bar", "baz"},
        "tabs, newlines and repeats collapse");

  const auto quoted = split_launch_arguments("--name \"bar baz\" -x");
  check(quoted == std::vector<std::string>{"--name", "bar baz", "-x"},
        "double quotes group tokens and are removed");

  const auto escaped = split_launch_arguments("\"a\\\"b\" c");
  check(escaped == std::vector<std::string>{"a\"b", "c"},
        "backslash escapes the next char inside quotes");

  const auto empty_quoted = split_launch_arguments("-x \"\" -y");
  check(empty_quoted == std::vector<std::string>{"-x", "-y"},
        "empty quoted string yields no token");

  const auto unterminated = split_launch_arguments("\"abc def");
  check(unterminated == std::vector<std::string>{"abc def"},
        "unterminated quote consumes to end of input");

  const auto literal_backslash = split_launch_arguments("a\\b c");
  check(literal_backslash == std::vector<std::string>{"a\\b", "c"},
        "backslash outside quotes is literal");
}

TEST_CASE("resolve launch cwd", "[engine]") {
  using engine::resolve_launch_cwd;
  const fs::path game_dir = fs::path("/games/skyrim");

  check(resolve_launch_cwd(game_dir, "").empty(), "empty start_in stays empty");
  check(resolve_launch_cwd(game_dir, "bin") == game_dir / "bin",
        "relative start_in joins under game_dir");
  check(resolve_launch_cwd(game_dir, "a/b") == game_dir / "a/b",
        "nested relative start_in joins under game_dir");
  check(resolve_launch_cwd(game_dir, "/abs/dir") == fs::path("/abs/dir"),
        "absolute start_in passes through");
}

TEST_CASE("lookup executable launch config", "[engine]") {
  using engine::lookup_executable_launch_config;

  const fs::path root     = fresh_root("lookup");
  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "SKSE");

  write_file(root / "instance.toml",
             "game_id = \"testgame\"\n"
             "executables = [\n"
             "  { path = \"SKSE/skse64_loader.exe\", title = \"SKSE\", args = "
             "\"-forcesteamloader --name \\\"my profile\\\"\", cwd = \"SKSE\", env = "
             "[\"WINEDEBUG=+file\", \"GMM_X=1\"] },\n"
             "  { path = \"other.exe\", title = \"Other\" },\n"
             "]\n");

  // Case-insensitive first match wins; args are split, cwd resolved.
  const auto hit = lookup_executable_launch_config(
      root, game_dir, game_dir / "skse" / "SKSE64_LOADER.exe");
  check(hit.found, "entry found case-insensitively");
  check(hit.args ==
            std::vector<std::string>{"-forcesteamloader", "--name", "my profile"},
        "entry args split with shell quoting");
  check(hit.environment == std::vector<std::string>{"WINEDEBUG=+file", "GMM_X=1"},
        "entry env forwarded verbatim");
  check(hit.cwd == game_dir / "SKSE", "relative entry cwd resolved under game_dir");

  // Entry without config still matches, with empty fields.
  const auto bare =
      lookup_executable_launch_config(root, game_dir, game_dir / "OTHER.EXE");
  check(bare.found, "config-less entry still matches");
  check(bare.args.empty(), "config-less entry has no args");
  check(bare.environment.empty(), "config-less entry has no env");
  check(bare.cwd.empty(), "config-less entry has no cwd");

  // Unknown binary never matches.
  const auto miss =
      lookup_executable_launch_config(root, game_dir, game_dir / "missing.exe");
  check(!miss.found, "unknown binary has no entry");
  check(miss.args.empty() && miss.environment.empty() && miss.cwd.empty(),
        "miss carries empty fields");

  // Binary outside the game dir never matches (no ".." escape).
  const auto outside =
      lookup_executable_launch_config(root, game_dir, root / "other.exe");
  check(!outside.found, "binary outside game_dir has no entry");

  fs::remove_all(root);
}

TEST_CASE("lookup executable launch config edge cases", "[engine]") {
  using engine::lookup_executable_launch_config;

  // Missing instance.toml behaves like no entries.
  const fs::path bare_root = fresh_root("missing_toml");
  const fs::path game_dir  = bare_root / "game";
  fs::create_directories(game_dir);
  const auto no_file =
      lookup_executable_launch_config(bare_root, game_dir, game_dir / "a.exe");
  check(!no_file.found, "missing instance.toml yields no entry");
  fs::remove_all(bare_root);

  // [[executables]] section style parses the same as inline tables, the
  // first matching entry wins, and absolute cwd passes through.
  const fs::path root = fresh_root("sections");
  const fs::path game = root / "game";
  fs::create_directories(game);
  write_file(root / "instance.toml", "game_id = \"testgame\"\n"
                                     "[[executables]]\n"
                                     "path = \"tool.exe\"\n"
                                     "args = \"-a\"\n"
                                     "cwd = \"/tmp/toolwork\"\n"
                                     "env = [\"K=V\"]\n"
                                     "[[executables]]\n"
                                     "path = \"tool.exe\"\n"
                                     "args = \"-b\"\n");
  const auto first = lookup_executable_launch_config(root, game, game / "TOOL.EXE");
  check(first.found, "section-style entry found");
  check(first.args == std::vector<std::string>{"-a"}, "first match wins");
  check(first.cwd == fs::path("/tmp/toolwork"), "absolute cwd passes through");
  check(first.environment == std::vector<std::string>{"K=V"}, "section env forwarded");
  fs::remove_all(root);

  // Legacy plain-string entries carry no config and are skipped.
  const fs::path legacy_root = fresh_root("legacy");
  const fs::path legacy_game = legacy_root / "game";
  fs::create_directories(legacy_game);
  write_file(legacy_root / "instance.toml", "game_id = \"testgame\"\n"
                                            "executables = [\"plain.exe\"]\n");
  const auto legacy = lookup_executable_launch_config(legacy_root, legacy_game,
                                                      legacy_game / "plain.exe");
  check(!legacy.found, "legacy plain-string entry yields no config");
  fs::remove_all(legacy_root);
}
