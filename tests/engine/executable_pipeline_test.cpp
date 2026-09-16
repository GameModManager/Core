// Tests for the pack-declared executable pipeline (Workspace-3k11).
//
// Covers: path/argv/env resolution, platform override merging, rules[]
// sequencing, setup auto-run selection, rerun detection, process running,
// output capture, and launcher LaunchParams conversion. Entries are built
// directly (archive parsing is covered by gmmpack_test).

#include "engine/gmmpack/executable_pipeline.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

#include <catch2/catch_test_macros.hpp>

namespace fs      = std::filesystem;
namespace gmmpack = engine::gmmpack;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static gmmpack::ExecutableEntry make_setup(const std::string& id)
{
  gmmpack::ExecutableEntry e;
  e.id            = id;
  e.source_mod_id = "toolmod";
  e.relative_path = "Tool/tool.exe";
  e.working_dir   = "Tool";
  e.role          = "setup";
  e.auto_run      = true;
  return e;
}

static gmmpack::ExecutableEntry make_launcher(const std::string& id)
{
  gmmpack::ExecutableEntry e;
  e.id            = id;
  e.source_mod_id = "gamemod";
  e.relative_path = "skse_loader.exe";
  e.role          = "launcher";
  return e;
}

static gmmpack::ManifestRule make_rule(const std::string& type, const std::string& from,
                                       const std::string& to)
{
  gmmpack::ManifestRule r;
  r.type = type;
  r.from = from;
  r.to   = to;
  return r;
}

// RAII scratch dir under the system temp root.
struct TempDir
{
  fs::path path;
  TempDir()
  {
    static std::atomic<int> counter{0};
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    // pid-scoped: ctest runs test binaries in parallel, so a bare counter
    // would collide across processes sharing the temp root.
    path = fs::temp_directory_path() / ("gmm-exe-pipeline-" + std::to_string(pid) +
                                        "-" + std::to_string(counter.fetch_add(1)));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

static void write_file(const fs::path& p, const std::string& content)
{
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  REQUIRE(!ec);
  std::ofstream out(p, std::ios::binary);
  REQUIRE(out.is_open());
  out << content;
}

// ---------------------------------------------------------------------------
// resolve_executables
// ---------------------------------------------------------------------------

TEST_CASE("resolve_executables computes absolute paths and argv",
          "[executable_pipeline]")
{
  gmmpack::Gmmpack pack;
  auto e      = make_setup("nemesis");
  e.arguments = {"-forceD3D9"};
  pack.executables.push_back(e);

  const auto resolved = gmmpack::resolve_executables(pack, fs::path("/mods"), "linux");
  REQUIRE(resolved.size() == 1);
  const auto& r = resolved[0];
  CHECK(r.executable_path == fs::path("/mods/toolmod/Tool/tool.exe"));
  CHECK(r.working_dir == fs::path("/mods/toolmod/Tool"));
  REQUIRE(r.argv.size() == 2);
  CHECK(r.argv[0] == "/mods/toolmod/Tool/tool.exe");
  CHECK(r.argv[1] == "-forceD3D9");
  CHECK(r.output_dir.empty());
}

TEST_CASE("resolve_executables falls back to exe parent dir without workingDir",
          "[executable_pipeline]")
{
  gmmpack::Gmmpack pack;
  auto e = make_setup("pandora");
  e.working_dir.clear();
  pack.executables.push_back(e);

  const auto resolved = gmmpack::resolve_executables(pack, fs::path("/mods"), "linux");
  REQUIRE(resolved.size() == 1);
  CHECK(resolved[0].working_dir == fs::path("/mods/toolmod/Tool"));
}

TEST_CASE("resolve_executables injects resolved output path via argName",
          "[executable_pipeline]")
{
  gmmpack::Gmmpack pack;
  auto e = make_setup("nemesis");
  gmmpack::ExecOutput out;
  out.path             = "Output";
  out.arg_name         = "--output";
  out.capture          = "syntheticMod";
  out.synthetic_mod_id = "nemesis-output";
  e.output             = out;
  pack.executables.push_back(e);

  const auto resolved = gmmpack::resolve_executables(pack, fs::path("/mods"), "linux");
  REQUIRE(resolved.size() == 1);
  const auto& r = resolved[0];
  CHECK(r.output_dir == fs::path("/mods/toolmod/Tool/Output"));
  REQUIRE(r.argv.size() == 3);
  CHECK(r.argv[1] == "--output");
  CHECK(r.argv[2] == "/mods/toolmod/Tool/Output");
}

TEST_CASE("resolve_executables skips output injection without argName",
          "[executable_pipeline]")
{
  gmmpack::Gmmpack pack;
  auto e = make_setup("nemesis");
  gmmpack::ExecOutput out;
  out.path    = "Output";
  out.capture = "inPlace";
  e.output    = out;
  pack.executables.push_back(e);

  const auto resolved = gmmpack::resolve_executables(pack, fs::path("/mods"), "linux");
  REQUIRE(resolved.size() == 1);
  CHECK(resolved[0].output_dir == fs::path("/mods/toolmod/Tool/Output"));
  CHECK(resolved[0].argv.size() == 1);  // no injection, fixed default kept
}

// ---------------------------------------------------------------------------
// merged_environment
// ---------------------------------------------------------------------------

TEST_CASE("merged_environment layers per-OS vars over the base set",
          "[executable_pipeline]")
{
  auto e     = make_setup("nemesis");
  e.env_vars = {{"BASE", "1"}, {"SHARED", "base"}};
  gmmpack::ExecPlatformOverride linux_override;
  linux_override.env_vars = {{"SHARED", "linux"}, {"WINEDEBUG", "+file"}};
  e.platform.linux_plat   = linux_override;
  gmmpack::ExecPlatformOverride win_override;
  win_override.env_vars = {{"WINONLY", "x"}};
  e.platform.windows    = win_override;

  const auto env = gmmpack::merged_environment(e, "linux");
  CHECK(env == std::vector<std::string>{"BASE=1", "SHARED=linux", "WINEDEBUG=+file"});

  const auto win = gmmpack::merged_environment(e, "windows");
  CHECK(win == std::vector<std::string>{"BASE=1", "SHARED=base", "WINONLY=x"});

  const auto mac = gmmpack::merged_environment(e, "macos");
  CHECK(mac == std::vector<std::string>{"BASE=1", "SHARED=base"});
}

// ---------------------------------------------------------------------------
// order_for_run
// ---------------------------------------------------------------------------

TEST_CASE("order_for_run honors before/after/requires", "[executable_pipeline]")
{
  const std::vector<std::string> ids = {"awesome-mod", "nemesis", "patcher"};
  const std::vector<gmmpack::ManifestRule> rules = {
      make_rule("requires", "nemesis", "awesome-mod"),  // mod first
      make_rule("before", "nemesis", "patcher"),
      make_rule("after", "patcher", "awesome-mod"),  // patcher after mod
  };
  const auto order = gmmpack::order_for_run(ids, rules);
  CHECK(!order.has_cycle);
  CHECK(order.ids == std::vector<std::string>{"awesome-mod", "nemesis", "patcher"});
}

TEST_CASE("order_for_run keeps pack order without constraints", "[executable_pipeline]")
{
  const std::vector<std::string> ids = {"b", "a", "c"};
  const auto order                   = gmmpack::order_for_run(ids, {});
  CHECK(!order.has_cycle);
  CHECK(order.ids == ids);
}

TEST_CASE("order_for_run ignores unknown ids, conflicts, and self rules",
          "[executable_pipeline]")
{
  const std::vector<std::string> ids             = {"a", "b"};
  const std::vector<gmmpack::ManifestRule> rules = {
      make_rule("before", "a", "ghost-mod"),  // references a plain mod id
      make_rule("conflicts", "a", "b"),       // no ordering implied
      make_rule("before", "a", "a"),          // self-rule
      make_rule("bogus", "a", "b"),           // unknown type
  };
  const auto order = gmmpack::order_for_run(ids, rules);
  CHECK(!order.has_cycle);
  CHECK(order.ids == ids);
}

TEST_CASE("order_for_run flags cycles and keeps every id", "[executable_pipeline]")
{
  const std::vector<std::string> ids             = {"a", "b", "c"};
  const std::vector<gmmpack::ManifestRule> rules = {
      make_rule("before", "a", "b"),
      make_rule("before", "b", "a"),  // contradiction
  };
  const auto order = gmmpack::order_for_run(ids, rules);
  CHECK(order.has_cycle);
  REQUIRE(order.ids.size() == 3);
  CHECK(order.ids[0] == "c");  // unconstrained id still runs first
}

// ---------------------------------------------------------------------------
// setup_auto_run_ids
// ---------------------------------------------------------------------------

TEST_CASE("setup_auto_run_ids selects setup auto-runs in run order",
          "[executable_pipeline]")
{
  gmmpack::Gmmpack pack;
  auto setup      = make_setup("nemesis");
  auto manual     = make_setup("bodyslide");
  manual.auto_run = false;  // registered, never auto-fired
  pack.executables.push_back(make_launcher("play"));
  pack.executables.push_back(manual);
  pack.executables.push_back(setup);
  pack.manifest.rules.push_back(make_rule("before", "nemesis", "bodyslide"));

  const std::vector<std::string> ids = {"play", "bodyslide", "nemesis", "skyui"};
  const auto order                   = gmmpack::order_for_run(ids, pack.manifest.rules);
  const auto due                     = gmmpack::setup_auto_run_ids(pack, order);
  // Launcher "play", manual "bodyslide", and plain mod "skyui" excluded.
  CHECK(due == std::vector<std::string>{"nemesis"});
}

// ---------------------------------------------------------------------------
// modset_input_hash + exe_rerun_due
// ---------------------------------------------------------------------------

TEST_CASE("modset_input_hash is stable and order-independent", "[executable_pipeline]")
{
  const std::vector<std::string> a = {"skyui:1.4.2", "awesome-mod:2.0"};
  const std::vector<std::string> b = {"awesome-mod:2.0", "skyui:1.4.2"};
  CHECK(gmmpack::modset_input_hash(a) == gmmpack::modset_input_hash(b));
  CHECK(gmmpack::modset_input_hash({}) == gmmpack::modset_input_hash({}));
  CHECK(gmmpack::modset_input_hash(a) !=
        gmmpack::modset_input_hash({"skyui:1.4.3", "awesome-mod:2.0"}));
}

TEST_CASE("exe_rerun_due fires only on real modset change", "[executable_pipeline]")
{
  auto e                   = make_setup("nemesis");
  e.rerun_on_modset_change = true;
  const std::string h1     = gmmpack::modset_input_hash({"a:1"});
  const std::string h2     = gmmpack::modset_input_hash({"a:2"});
  CHECK(gmmpack::exe_rerun_due(e, h2, h1));
  CHECK(!gmmpack::exe_rerun_due(e, h1, h1));  // unchanged
  CHECK(!gmmpack::exe_rerun_due(e, h2, ""));  // first install: auto-run path
  e.rerun_on_modset_change = false;
  CHECK(!gmmpack::exe_rerun_due(e, h2, h1));  // opted out
}

// ---------------------------------------------------------------------------
// run_executable (POSIX only - Windows run_captured is a stub)
// ---------------------------------------------------------------------------

#ifndef _WIN32

TEST_CASE("run_executable captures output with env and cwd", "[executable_pipeline]")
{
  TempDir tmp;
  const fs::path tool_dir = tmp.path / "toolmod" / "Tool";
  write_file(tool_dir / "probe.sh",
             "#!/bin/sh\npwd\necho \"var=$PROBE_VAR\"\necho oops >&2\n");

  gmmpack::Gmmpack pack;
  auto e          = make_setup("probe");
  e.relative_path = "probe.sh";
  e.working_dir   = "Tool";
  e.env_vars      = {{"PROBE_VAR", "hello"}};
  pack.executables.push_back(e);

  const auto resolved =
      gmmpack::resolve_executables(pack, tmp.path / "toolmod", "linux");
  REQUIRE(resolved.size() == 1);
  // resolve against the real temp root (symlink-free for the cwd check)
  gmmpack::ResolvedExecutable r = resolved[0];
  r.executable_path             = tool_dir / "probe.sh";
  r.working_dir                 = tool_dir;
  r.argv[0]                     = r.executable_path.string();

  const engine::CapturedProcess proc = gmmpack::run_executable(r);
  REQUIRE(proc.ok);
  CHECK(proc.exit_code == 0);
  CHECK(proc.out.find(tool_dir.string()) != std::string::npos);  // cwd took
  CHECK(proc.out.find("var=hello") != std::string::npos);        // env took
  CHECK(proc.err == "oops\n");
}

TEST_CASE("run_executable propagates non-zero exit codes", "[executable_pipeline]")
{
  TempDir tmp;
  write_file(tmp.path / "fail.sh", "#!/bin/sh\nexit 3\n");

  gmmpack::ResolvedExecutable r;
  r.executable_path = tmp.path / "fail.sh";
  r.working_dir     = tmp.path;
  r.argv            = {r.executable_path.string()};

  const engine::CapturedProcess proc = gmmpack::run_executable(r);
  REQUIRE(proc.ok);
  CHECK(proc.exit_code == 3);
}

TEST_CASE("run_executable reports missing executables via exit 127",
          "[executable_pipeline]")
{
  gmmpack::ResolvedExecutable r;
  r.executable_path = "/nonexistent-tool-dir/tool";
  r.working_dir     = fs::temp_directory_path();
  r.argv            = {r.executable_path.string()};

  const engine::CapturedProcess proc = gmmpack::run_executable(r);
  REQUIRE(proc.ok);              // fork+wait succeeded...
  CHECK(proc.exit_code == 127);  // ...but exec failed
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// capture_exe_output
// ---------------------------------------------------------------------------

TEST_CASE("capture_exe_output promotes output to a synthetic mod",
          "[executable_pipeline]")
{
  TempDir tmp;
  const fs::path mods = tmp.path / "mods";
  const fs::path out  = tmp.path / "toolmod" / "Tool" / "Output";
  write_file(out / "meshes" / "gen.nif", "generated");
  write_file(out / "skse" / "gen.ini", "settings");
  // Stale content from a previous run must be fully replaced.
  write_file(mods / "nemesis-output" / "stale.txt", "stale");

  gmmpack::ResolvedExecutable r;
  r.entry = make_setup("nemesis");
  gmmpack::ExecOutput o;
  o.path             = "Output";
  o.capture          = "syntheticMod";
  o.synthetic_mod_id = "nemesis-output";
  r.entry.output     = o;
  r.output_dir       = out;

  const auto cap = gmmpack::capture_exe_output(r, mods);
  CHECK(cap.error.empty());
  REQUIRE(cap.ok);
  CHECK(fs::is_regular_file(mods / "nemesis-output" / "meshes" / "gen.nif"));
  CHECK(fs::is_regular_file(mods / "nemesis-output" / "skse" / "gen.ini"));
  CHECK(!fs::exists(mods / "nemesis-output" / "stale.txt"));
}

TEST_CASE("capture_exe_output inPlace and missing output are no-ops",
          "[executable_pipeline]")
{
  TempDir tmp;
  gmmpack::ResolvedExecutable r;
  r.entry = make_setup("tool");
  gmmpack::ExecOutput o;
  o.path         = "Output";
  o.capture      = "inPlace";
  r.entry.output = o;
  r.output_dir   = tmp.path / "Output";

  CHECK(gmmpack::capture_exe_output(r, tmp.path).ok);

  gmmpack::ResolvedExecutable plain;
  plain.entry = make_setup("plain");
  CHECK(gmmpack::capture_exe_output(plain, tmp.path).ok);
}

TEST_CASE("capture_exe_output rejects bad syntheticMod targets",
          "[executable_pipeline]")
{
  TempDir tmp;
  gmmpack::ResolvedExecutable r;
  r.entry = make_setup("nemesis");
  gmmpack::ExecOutput o;
  o.path         = "Output";
  o.capture      = "syntheticMod";
  r.entry.output = o;
  r.output_dir   = tmp.path / "Output";
  fs::create_directories(r.output_dir);

  // Missing syntheticModId.
  CHECK(!gmmpack::capture_exe_output(r, tmp.path).ok);

  // Path traversal in the id.
  r.entry.output->synthetic_mod_id = "../escape";
  CHECK(!gmmpack::capture_exe_output(r, tmp.path).ok);

  // Missing output dir with a valid id.
  r.entry.output->synthetic_mod_id = "nemesis-output";
  r.output_dir                     = tmp.path / "Nope";
  CHECK(!gmmpack::capture_exe_output(r, tmp.path).ok);
}

// ---------------------------------------------------------------------------
// to_launch_params
// ---------------------------------------------------------------------------

TEST_CASE("to_launch_params converts a launcher exe for the Play path",
          "[executable_pipeline]")
{
  gmmpack::Gmmpack pack;
  auto e        = make_launcher("play");
  e.arguments   = {"-forcesteamloader"};
  e.env_vars    = {{"WINEDEBUG", "-all"}};
  e.working_dir = "root";
  pack.executables.push_back(e);

  const auto resolved = gmmpack::resolve_executables(pack, fs::path("/mods"), "linux");
  REQUIRE(resolved.size() == 1);
  const engine::LaunchParams params = gmmpack::to_launch_params(resolved[0]);
  CHECK(params.executable == fs::path("/mods/gamemod/skse_loader.exe"));
  CHECK(params.args == std::vector<std::string>{"-forcesteamloader"});
  CHECK(params.environment == std::vector<std::string>{"WINEDEBUG=-all"});
  CHECK(params.cwd == fs::path("/mods/gamemod/root"));
}
