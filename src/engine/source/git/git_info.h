#pragma once

#include <filesystem>
#include <string>

// Pure-facts about a mod folder that is a git working copy. Every function
// here is a filesystem read: no process is spawned, so calling it during a
// scan or a list refresh costs a stat (and one small file read when the
// folder really is a repository). Running git - fetch, pull, reset - lives in
// the UI layer, which is the only place that can prompt first.
namespace engine::Git {

// True when `dir` is the working copy of a repository. Matches a `.git`
// directory (a plain clone) and a `.git` file (a worktree or a submodule,
// which stores "gitdir: <path>" instead of the directory itself).
[[nodiscard]] bool is_repository(const std::filesystem::path &dir);

// Origin remote URL recorded in `.git/config`, or an empty string when there
// is no repository or no `origin` remote. For a worktree/submodule the
// `gitdir:` indirection in the `.git` file is followed. GitHub URLs are
// returned verbatim, including the .git suffix and any credentials a user
// embedded in the URL.
[[nodiscard]] std::string remote_url(const std::filesystem::path &dir);

// Host of a git remote URL, lowercased and without user or port:
//   https://github.com/a/b.git  -> github.com
//   git@gitlab.com:group/proj  -> gitlab.com
//   ssh://git@host:2222/p.git  -> host
// Empty when the URL carries no host.
[[nodiscard]] std::string host_of(const std::string &url);

}  // namespace engine::Git
