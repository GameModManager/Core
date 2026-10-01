// Settings > Workarounds > Miscellaneous: the two skip lists must actually
// stop files from reaching the game, not merely round-trip through QSettings.
//
// Each case drives a real consumer and asserts on the CONSUMER's output:
//
//   "Skip file suffixes"  -> engine::is_hidden_file(), which every
//       deploy / Data tab / mod scanner / mod info file listing already
//       routes through. A listed suffix is treated exactly like the built-in
//       .gmmhidden marker: deployed file is absent from staging, and the mod
//       scanner marks the mod as having hidden files.
//
//   "Skip directories" -> the two tree walks that decide what a mod
//       contributes: the deploy walk and ConflictEngine::walk_mod. A listed
//       directory name is not descended into, so nothing under it is staged
//       and nothing under it appears in the conflict registry (which is the
//       same verdict, since the game could never have seen the file).
//
// Both cases also pin the untouched-install behaviour: with the lists at their
// stored default (empty) every file is deployed and every file is in the
// registry.
#include "engine/core/util/fs_utils.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/index/conflict_engine.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {

void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

void write_file(const fs::path &p, const std::string &contents = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << contents;
  if (!out.good()) {
    std::printf("FAIL: could not write %s\n", p.string().c_str());
    std::exit(1);
  }
}

// One mod holding a plain file, a ".skipme" file, a ".git/config" file and a
// "docs/keep.txt" file, so a single deploy and a single conflict scan can
// observe all four outcomes at once.
void make_instance(const fs::path &root) {
  const fs::path mod = root / "mods" / "ModA";
  write_file(mod / "SkyUI.esp");
  write_file(mod / "notes.skipme");
  write_file(mod / ".git" / "config");
  write_file(mod / "docs" / "keep.txt");

  // Nothing outside .git, so skipping that one directory empties the mod.
  write_file(root / "mods" / "GitOnly" / ".git" / "HEAD");
}

fs::path case_root(const char *name) {
  const fs::path root =
      fs::current_path() / (std::string(name) + "_" + std::to_string(getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  return root;
}

bool staged(const fs::path &staging, const char *rel) {
  return fs::exists(staging / "Data" / rel);
}

}  // namespace

TEST_CASE("workarounds: skip lists reach deploy, conflict scan and the scanner",
          "[engine][settings]") {
  const fs::path root = case_root("gmm_workarounds");
  make_instance(root);
  const fs::path mods_dir = root / "mods";

  // --- default (untouched install): nothing is skipped ---------------------
  engine::set_workarounds(engine::Workarounds{});
  check(!engine::is_hidden_file("notes.skipme"),
        "with the list empty a listed-elsewhere suffix is not hidden");
  check(!engine::is_skipped_directory(".git"),
        "with the list empty a directory is not skipped");

  const fs::path staging_default = root / "staging_default";
  check(engine::deploy_all_enabled_mods(mods_dir, staging_default, "Data",
                                        /*deploy_include_mod_id=*/false, ""),
        "deploy succeeds with the default (empty) workarounds");
  check(staged(staging_default, "SkyUI.esp"), "default: the plain file deploys");
  check(staged(staging_default, "notes.skipme"), "default: the .skipme file deploys");
  check(staged(staging_default, ".git/config"),
        "default: the file inside .git deploys");
  check(staged(staging_default, "docs/keep.txt"),
        "default: the file in an unlisted directory deploys");

  engine::ConflictEngine all_files;
  const auto default_stats =
      all_files.compute(mods_dir, {{"ModA", 0}, {"GitOnly", 1}}, "", "",
                        /*conflict_reversed=*/false);
  check(default_stats.count("ModA") == 1, "default: the mod is in the stats map");
  {
    bool saw_skipme = false;
    bool saw_git    = false;
    for (const auto &[path, owners] : all_files.last_registry()) {
      (void)owners;
      if (path == "notes.skipme")
        saw_skipme = true;
      if (path == ".git/config")
        saw_git = true;
    }
    check(saw_skipme, "default: .skipme file is in the conflict registry");
    check(saw_git, "default: .git/config is in the conflict registry");
  }

  // --- "Skip file suffixes" -> is_hidden_file(), so deploy drops the file ---
  engine::Workarounds w;
  w.hidden_suffixes = {".skipme"};
  w.skipped_dirs    = {".git/"};
  engine::set_workarounds(w);

  check(engine::is_hidden_file("notes.skipme"),
        "a listed suffix is treated like the built-in hidden markers");
  check(engine::is_hidden_file("Data/notes.skipme"),
        "the suffix is matched on the basename at any depth");
  check(engine::is_hidden_file("NOTES.SKIPME"),
        "the suffix is matched case-insensitively");
  check(!engine::is_hidden_file("SkyUI.esp"), "an unlisted file is unaffected");
  check(!engine::is_hidden_file("skipme"), "a file named exactly like the "
                                           "suffix is not hidden");
  check(engine::is_skipped_directory(".git"),
        "a listed directory matches, trailing slash included");
  check(engine::is_skipped_directory(".GIT"),
        "a listed directory matches case-insensitively");
  check(!engine::is_skipped_directory("docs"), "an unlisted directory does not match");
  check(!engine::is_skipped_directory("Data/.git"),
        "only a bare directory name matches, never a path");

  const fs::path staging = root / "staging";
  check(engine::deploy_all_enabled_mods(mods_dir, staging, "Data",
                                        /*deploy_include_mod_id=*/false, ""),
        "deploy succeeds with the skip lists set");
  check(staged(staging, "SkyUI.esp"), "an unskipped file still deploys");
  check(!staged(staging, "notes.skipme"), "a skipped suffix does not reach the game");
  check(!staged(staging, ".git/config"),
        "a skipped directory is not descended into, so nothing in it deploys");
  check(staged(staging, "docs/keep.txt"),
        "a directory that is not on the list is untouched");

  // --- "Skip directories" -> the conflict engine walk ----------------------
  engine::ConflictEngine filtered;
  filtered.compute(mods_dir, {{"ModA", 0}}, "", "", /*conflict_reversed=*/false);
  for (const auto &[path, _owners] : filtered.last_registry())
    CHECK(path != ".git/config");
  bool saw_plain = false;
  for (const auto &[path, _owners] : filtered.last_registry())
    if (path == "docs/keep.txt")
      saw_plain = true;
  check(saw_plain, "an unskipped file is still in the conflict registry");
  // Deliberate, and the same for the built-in markers: the conflict engine
  // counts a file whether or not deploy will copy it, so a configured suffix
  // still raises a conflict. The skip lists decide what reaches the game and
  // what the file views list, not what collides.
  bool saw_skipme = false;
  for (const auto &[path, _owners] : filtered.last_registry())
    if (path == "notes.skipme")
      saw_skipme = true;
  check(saw_skipme,
        "a skipped suffix is still counted as a conflict, as .gmmhidden is");

  // --- the mod scanner reads the same two verdicts ------------------------
  engine::GameKnowledge knowledge;
  const auto scanned = engine::ModScanner::scan_dir(knowledge, "testgame", mods_dir);
  REQUIRE(scanned.size() == 2);
  bool mod_a_hidden    = false;
  bool mod_a_not_empty = false;
  for (const auto &m : scanned) {
    if (m.folder_name == "ModA") {
      mod_a_hidden    = m.has_hidden_files;
      mod_a_not_empty = !m.is_empty;
    }
  }
  check(mod_a_hidden,
        "the mod scanner sees a .skipme file as hidden (the hidden-files badge)");
  check(mod_a_not_empty,
        "the mod scanner still sees real content through the skipped .git");

  engine::set_workarounds(engine::Workarounds{});
  engine::Workarounds skip_only_dir;
  skip_only_dir.skipped_dirs = {".git"};
  engine::set_workarounds(skip_only_dir);
  const auto dir_scanned =
      engine::ModScanner::scan_dir(knowledge, "testgame", mods_dir);
  bool git_only_empty    = false;
  bool mod_a_has_content = false;
  for (const auto &m : dir_scanned) {
    if (m.folder_name == "GitOnly")
      git_only_empty = m.is_empty;
    if (m.folder_name == "ModA")
      mod_a_has_content = !m.is_empty;
  }
  check(git_only_empty,
        "a mod whose only file sits in a skipped directory reads as empty, the "
        "same verdict deploy gives it");
  check(mod_a_has_content, "a mod with content elsewhere is unaffected");

  // --- restore the process-wide default so no later case inherits it -------
  engine::set_workarounds(engine::Workarounds{});
  fs::remove_all(root);
}