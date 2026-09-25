// Headless CLI launch args/env/cwd (Workspace-cch0).
//   - Parser: --args/--env (repeatable)/--cwd land in ParsedArgs with
//     explicit has_* flags; bare headless invocations set none.
//   - Mapping: cli::build_launch_request assembles the LaunchPrepRequest from
//     the instance.toml executables entry (GUI parity default) with explicit
//     CLI flags winning per field; prepare_launch_params forwards the result
//     to LaunchParams verbatim. No process is ever spawned here.
// Hermetic: QCoreApplication per case (one CTest entry per process),
// QT_QPA_PLATFORM=offscreen via the test property, fixtures under temp dir.
#include "cli/command_line.h"
#include "cli/headless_launcher.h"

#include <QByteArray>
#include <QCoreApplication>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/instance/instance_utils.h"
#include "engine/deploy/launch/launcher.h"
#include "engine/game/registry/game_knowledge.h"

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

cli::ParsedArgs parse_argv(const std::vector<std::string> &words) {
  std::vector<QByteArray> storage;
  storage.reserve(words.size());
  for (const auto &w : words)
    storage.emplace_back(w.c_str());
  std::vector<char *> argv;
  argv.reserve(storage.size());
  for (auto &b : storage)
    argv.push_back(b.data());
  int argc = static_cast<int>(argv.size());
  QCoreApplication app(argc, argv.data());
  cli::CommandLine cmd(argc, argv.data());
  REQUIRE(cmd.parse());
  return cmd.args();
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
      ("gmm_test_headless_cli_" + std::to_string(getpid()) + "_" + name);
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

engine::GameKnowledge symlink_knowledge() {
  engine::GameKnowledge knowledge;
  knowledge.set("testgame", "deploy_prefix", "Data");
  knowledge.set("testgame", "deploy_include_mod_id", "false");
  knowledge.set("testgame", "disable_mechanism", "disabled.txt");
  knowledge.set("testgame", "deploy_strategy", "symlink");
  return knowledge;
}
}  // namespace

TEST_CASE("headless CLI flags parse", "[cli]") {
  const auto args =
      parse_argv({"gmm", "--launch", "--instance", "MyInst", "--exe",
                  "skse64_loader.exe", "--args", "-foo \"bar baz\"", "--env",
                  "WINEDEBUG=+file", "--env", "GMM_X=1", "--cwd", "bin"});
  check(args.headless, "--launch sets headless");
  check(args.instance_name == "MyInst", "--instance parsed");
  check(args.exe_path == "skse64_loader.exe", "--exe parsed");
  check(args.has_launch_args, "--args marks has_launch_args");
  check(args.launch_args == "-foo \"bar baz\"", "--args value kept verbatim");
  check(args.has_launch_env, "--env marks has_launch_env");
  check(args.launch_env == QStringList({"WINEDEBUG=+file", "GMM_X=1"}),
        "repeatable --env collects every occurrence in order");
  check(args.has_launch_cwd, "--cwd marks has_launch_cwd");
  check(args.launch_cwd == "bin", "--cwd value parsed");
}

TEST_CASE("headless CLI bare launch has no overrides", "[cli]") {
  const auto args =
      parse_argv({"gmm", "--launch", "--instance", "MyInst", "--exe", "game.exe"});
  check(args.headless, "--launch sets headless");
  check(!args.has_launch_args, "no --args means no override");
  check(!args.has_launch_env, "no --env means no override");
  check(!args.has_launch_cwd, "no --cwd means no override");
  check(args.launch_args.isEmpty(), "launch_args empty by default");
  check(args.launch_env.isEmpty(), "launch_env empty by default");
  check(args.launch_cwd.isEmpty(), "launch_cwd empty by default");
}

TEST_CASE("headless launch request uses entry defaults", "[cli]") {
  const fs::path root     = fresh_root("entry");
  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "bin");
  fs::create_directories(root / "mods");
  write_file(root / "instance.toml",
             "game_id = \"testgame\"\n"
             "executables = [\n"
             "  { path = \"game.exe\", args = \"-a \\\"b c\\\"\", cwd = \"bin\", env = "
             "[\"K=V\"] },\n"
             "]\n");

  cli::HeadlessLauncher::Config config;
  config.executable    = game_dir / "game.exe";
  config.game_dir      = game_dir;
  config.instance_root = root;
  config.knowledge     = nullptr;
  config.game_id       = "testgame";

  const engine::LaunchPrepRequest req = cli::build_launch_request(config);
  check(req.args == std::vector<std::string>{"-a", "b c"},
        "entry args split into the request");
  check(req.environment == std::vector<std::string>{"K=V"},
        "entry env forwarded into the request");
  check(req.cwd == game_dir / "bin", "entry cwd resolved under game_dir");

  // The request rides the shared path to the launcher boundary unchanged.
  engine::LaunchPrepRequest owned = req;
  owned.knowledge                 = symlink_knowledge();
  const auto params               = engine::prepare_launch_params(owned);
  check(params.args == req.args, "args reach LaunchParams");
  check(params.environment == req.environment, "env reaches LaunchParams");
  check(params.cwd == req.cwd, "cwd reaches LaunchParams");

  fs::remove_all(root);
}

TEST_CASE("headless CLI flags win over entry", "[cli]") {
  const fs::path root     = fresh_root("precedence");
  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir / "bin");
  fs::create_directories(game_dir / "custom");
  fs::create_directories(root / "mods");
  write_file(root / "instance.toml",
             "game_id = \"testgame\"\n"
             "executables = [\n"
             "  { path = \"game.exe\", args = \"-entry\", cwd = \"bin\", env = "
             "[\"ENTRY=1\"] },\n"
             "]\n");

  cli::HeadlessLauncher::Config config;
  config.executable      = game_dir / "game.exe";
  config.game_dir        = game_dir;
  config.instance_root   = root;
  config.game_id         = "testgame";
  config.args            = {"-flag"};
  config.args_set        = true;
  config.environment     = {"FLAG=1"};
  config.environment_set = true;
  config.cwd             = "custom";
  config.cwd_set         = true;

  const engine::LaunchPrepRequest req = cli::build_launch_request(config);
  check(req.args == std::vector<std::string>{"-flag"}, "explicit args win");
  check(req.environment == std::vector<std::string>{"FLAG=1"}, "explicit env wins");
  check(req.cwd == game_dir / "custom", "explicit cwd wins and resolves");

  // Partial override: only args set, env/cwd still come from the entry.
  cli::HeadlessLauncher::Config partial = config;
  partial.environment_set               = false;
  partial.environment.clear();
  partial.cwd_set = false;
  partial.cwd.clear();
  const engine::LaunchPrepRequest preq = cli::build_launch_request(partial);
  check(preq.args == std::vector<std::string>{"-flag"}, "partial args win");
  check(preq.environment == std::vector<std::string>{"ENTRY=1"},
        "entry env kept without --env");
  check(preq.cwd == game_dir / "bin", "entry cwd kept without --cwd");

  fs::remove_all(root);
}

TEST_CASE("headless launch request empty regression", "[cli]") {
  const fs::path root     = fresh_root("empty");
  const fs::path game_dir = root / "game";
  fs::create_directories(game_dir);
  fs::create_directories(root / "mods");
  write_file(root / "instance.toml", "game_id = \"testgame\"\n");

  cli::HeadlessLauncher::Config config;
  config.executable    = game_dir / "game.exe";
  config.game_dir      = game_dir;
  config.instance_root = root;
  config.game_id       = "testgame";

  const engine::LaunchPrepRequest req = cli::build_launch_request(config);
  check(req.args.empty(), "no entry and no flags means no args");
  check(req.environment.empty(), "no entry and no flags means no env");
  check(req.cwd.empty(), "no entry and no flags means empty cwd");

  engine::LaunchPrepRequest owned = req;
  owned.knowledge                 = symlink_knowledge();
  const auto params               = engine::prepare_launch_params(owned);
  check(params.args.empty() && params.environment.empty() && params.cwd.empty(),
        "empty request stays empty at the launcher boundary");

  fs::remove_all(root);
}
