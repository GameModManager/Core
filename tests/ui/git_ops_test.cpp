// git_ops_test.cpp - the real-git behaviour of the Git source panel's actions.
//
// These assertions only mean something against a REAL repository and a REAL
// git binary, so this test runs git rather than mocking it: the load-bearing
// claims are (a) a mod folder is only ever reset when it IS its own
// repository root, and (b) the reset discards exactly what it said it would
// and nothing else. A mock would assert the mock.
//
// Skips cleanly (no failure) when git is not on PATH.
//
// Hermetic: every repo lives under std::filesystem::temp_directory_path(),
// no network - "upstream" is a local clone.
#include "ui/modinfo/git_ops.h"

#include <QApplication>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {

bool git_on_path() {
  return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

int sh(const fs::path &dir, const QStringList &args) {
  QProcess p;
  p.setWorkingDirectory(QString::fromStdString(dir.string()));
  p.start(QStandardPaths::findExecutable(QStringLiteral("git")), args);
  if (!p.waitForFinished(20000))
    return -1;
  return p.exitCode();
}

void write(const fs::path &p, const std::string &content) {
  std::ofstream f(p);
  f << content;
}

std::string read_file(const fs::path &p) {
  std::ifstream f(p);
  return std::string(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("git ops run against a real mod repository", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  if (!git_on_path()) {
    WARN("git is not on PATH - the git source actions cannot be exercised");
    return;
  }

  const fs::path root = fs::temp_directory_path() / "gmm_git_ops_test";
  fs::remove_all(root);
  fs::create_directories(root);
  REQUIRE(ui::gitops::available());

  // An "upstream" repository, and a clone of it that stands in for a mod
  // folder. Local paths only, so nothing here touches the network.
  const fs::path upstream = root / "upstream";
  REQUIRE(sh(root, {"init", "--quiet", "upstream"}) == 0);
  fs::create_directories(upstream / "Data");
  write(upstream / "Data" / "plugin.esp", "v1\n");
  REQUIRE(sh(upstream, {"config", "user.email", "t@t"}) == 0);
  REQUIRE(sh(upstream, {"config", "user.name", "t"}) == 0);
  REQUIRE(sh(upstream, {"add", "-A"}) == 0);
  REQUIRE(sh(upstream, {"commit", "--quiet", "-m", "one"}) == 0);

  const fs::path mod = root / "mod";
  REQUIRE(sh(root, {"clone", "--quiet", "upstream", "mod"}) == 0);

  // --- Scope: the mod folder IS its own repository root. ---
  CHECK(ui::gitops::is_scoped_repository(mod));

  // A subdirectory of the repo is NOT a repository at all - the destructive
  // path must refuse it rather than reaching up to the parent.
  fs::create_directories(mod / "sub");
  CHECK(!ui::gitops::is_scoped_repository(mod / "sub"));

  // A folder that is not a repo at all.
  const fs::path plain = root / "plain";
  fs::create_directories(plain);
  CHECK(!ui::gitops::is_scoped_repository(plain));

  // --- Status: branch, commit and a clean, up-to-date upstream. ---
  {
    const auto s = ui::gitops::status(mod);
    CHECK(s.ok);
    CHECK((s.branch == QLatin1String("main") || s.branch == QLatin1String("master")));
    CHECK(s.commit.size() >= 7);
    CHECK(s.has_upstream);
    CHECK(s.ahead == 0);
    CHECK(s.behind == 0);
  }

  // --- Upstream branches are listed, no network involved. ---
  CHECK(!ui::gitops::upstream_branches(mod).isEmpty());

  // --- A fast-forward pull brings the mod forward. ---
  write(upstream / "Data" / "plugin.esp", "v2\n");
  write(upstream / "Data" / "new.txt", "added\n");
  REQUIRE(sh(upstream, {"add", "-A"}) == 0);
  REQUIRE(sh(upstream, {"commit", "--quiet", "-m", "two"}) == 0);

  // Before the fetch, the local repo legitimately has no new upstream to see.
  {
    const auto s = ui::gitops::status(mod);
    CHECK(s.has_upstream);
    CHECK(s.behind == 0);
  }
  CHECK(ui::gitops::fetch(mod).ok);
  {
    const auto s = ui::gitops::status(mod);
    CHECK(s.behind == 1);
    CHECK(s.ahead == 0);
  }
  CHECK(ui::gitops::pull_ff(mod).ok);
  {
    const auto s = ui::gitops::status(mod);
    CHECK(s.behind == 0);
    CHECK(read_file(mod / "Data" / "new.txt") == "added\n");
  }

  // --- A diverged branch: pull refuses rather than merging, and says so. ---
  write(mod / "Data" / "plugin.esp", "local\n");
  REQUIRE(sh(mod, {"commit", "--quiet", "-am", "local work"}) == 0);
  write(upstream / "Data" / "plugin.esp", "v3\n");
  REQUIRE(sh(upstream, {"commit", "--quiet", "-am", "three"}) == 0);
  CHECK(ui::gitops::fetch(mod).ok);
  {
    const auto s = ui::gitops::status(mod);
    CHECK(s.ahead == 1);
    CHECK(s.behind == 1);
  }
  const auto refused = ui::gitops::pull_ff(mod);
  CHECK(!refused.ok);
  CHECK(!refused.summary().isEmpty());
  // Nothing was merged behind the user's back.
  CHECK(!fs::exists(mod / ".git" / "MERGE_HEAD"));

  // --- The destructive path: it says what it will discard, and does exactly
  // that, without touching the mod's own metadata. ---
  write(mod / "Data" / "local-edit.txt", "mine\n");
  fs::create_directories(mod / "junk");
  write(mod / "junk" / "stray.txt", "stray\n");
  write(mod / "meta.ini", "[GameModManager]\nsource_type=nexus\n");
  const int discardable = ui::gitops::discardable_change_count(mod);
  CHECK(discardable >= 2);

  const auto reset = ui::gitops::reset_to_upstream(mod);
  CHECK(reset.ok);
  // Tracked content now matches upstream, and the local commit is gone.
  CHECK(read_file(mod / "Data" / "plugin.esp") == "v3\n");
  CHECK(ui::gitops::status(mod).ahead == 0);
  CHECK(ui::gitops::status(mod).behind == 0);
  // Untracked files went, as the dialog said they would.
  CHECK(!fs::exists(mod / "junk" / "stray.txt"));
  CHECK(!fs::exists(mod / "Data" / "local-edit.txt"));
  // GMM's own sidecar survives: a bare `git clean -fd` would have eaten it.
  CHECK(fs::exists(mod / "meta.ini"));
  CHECK(ui::gitops::discardable_change_count(mod) == 0);

  // --- The guard is re-checked inside the destructive call, not only at the
  // button: pointed at a subdirectory it refuses without acting. ---
  write(mod / "Data" / "keepme.txt", "local\n");
  const auto blocked = ui::gitops::reset_to_upstream(mod / "Data");
  CHECK(!blocked.ok);
  CHECK(blocked.error.contains(QLatin1String("Refusing")));
  CHECK(fs::exists(mod / "Data" / "keepme.txt"));

  fs::remove_all(root);
}
