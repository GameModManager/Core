#pragma once

#include <QString>
#include <QStringList>

#include <filesystem>

// Running git for a single mod folder. Every call is scoped to one mod's own
// directory and reads git's real exit status, stdout and stderr - nothing
// here infers success from the absence of an error message.
//
// Lives in the UI layer, not the engine: the destructive operation
// (reset_to_upstream) must be reachable only from a place that can ask the
// user first, and the engine is Qt-free.
namespace ui::gitops {

struct Result {
  bool ok       = false;  // process started AND exited 0
  bool started  = false;  // the git binary was found and launched
  int exit_code = -1;
  QString out;    // git's stdout (rev-parse output, branch lists)
  QString error;  // git's stderr, or why it could not run at all

  // One line fit for a message box: git's first stderr line, falling back to
  // stdout, or a synthesized reason when git never ran.
  [[nodiscard]] QString summary() const;
};

// Current checkout state of `dir`. `has_upstream` false means the branch has
// no upstream configured - there is nothing to compare against, so ahead /
// behind stay 0 and the panel says so instead of claiming "up to date".
struct Status {
  bool ok = false;
  QString branch;
  QString commit;
  QString remote;  // origin URL, empty when the repo has none
  bool has_upstream = false;
  int ahead         = 0;
  int behind        = 0;
  QString error;
};

// True when a git binary is on PATH. Cached after the first call; the panel
// reports the miss in the UI rather than silently doing nothing.
[[nodiscard]] bool available();

// Run git with `-C .` inside `dir`. Never throws and never blocks longer than
// `timeout_ms`.
Result run(const std::filesystem::path &dir, const QStringList &args,
           int timeout_ms = 120000);

// True when `dir` is safe to hand to the destructive operations below:
// the canonical path holds a .git DIRECTORY (a worktree/submodule .git file
// points git at a gitdir outside the mod, so it is refused) and git agrees the
// repository's toplevel is exactly that directory. A mod folder nested inside
// some larger repository fails the second check, which is the point: reset and
// clean must never reach past the mod's own folder.
[[nodiscard]] bool is_scoped_repository(const std::filesystem::path &dir);

[[nodiscard]] Status status(const std::filesystem::path &dir);

// Remote branches known locally (no network), for the branch list.
[[nodiscard]] QStringList upstream_branches(const std::filesystem::path &dir);

// Number of local changes `reset_to_upstream` would actually destroy:
// modified, deleted and untracked entries from `git status --porcelain`,
// minus the mod's own metadata files (which the reset's clean explicitly
// spares). This is the number the confirmation shows, so it must be the
// number the reset really takes - not one the user later finds inflated.
[[nodiscard]] int discardable_change_count(const std::filesystem::path &dir);

// Fetch the remote (network). Leaves the working tree untouched.
Result fetch(const std::filesystem::path &dir);

// Fast-forward only. Refuses to merge, so a diverged branch fails loudly
// instead of leaving the user with a merge conflict they did not ask for.
Result pull_ff(const std::filesystem::path &dir);

// fetch, then hard-reset the branch to its upstream and delete untracked
// files - the mod folder ends up matching upstream. DESTRUCTIVE: it discards
// every local modification in the mod folder. The caller must confirm with the
// user first; is_scoped_repository() is re-checked here so no other caller can
// skip it.
Result reset_to_upstream(const std::filesystem::path &dir);
}  // namespace ui::gitops
