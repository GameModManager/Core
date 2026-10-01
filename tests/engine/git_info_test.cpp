// Reading a mod folder's git origin, against the config format git actually
// writes: TABBED indentation, a QUOTED subsection name (`[remote "origin"]`),
// and third-party keys mixed in with git's own.
#include "engine/source/git/git_info.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir() {
  const std::string name = "gmm_git_info_" + std::to_string(getpid());
  auto dir               = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// A plain clone: a `.git` directory holding a `config` of the given text.
fs::path write_plain_clone(const fs::path &parent, const std::string &folder,
                           const std::string &config) {
  const fs::path dir = parent / folder;
  fs::create_directories(dir / ".git");
  std::ofstream out(dir / ".git" / "config");
  out << config;
  return dir;
}

}  // namespace

// A mod folder under a game path containing SPACES reads its origin back.
// The path shape is not incidental: mods live under
// "<Steam>/steamapps/common/The Binding of Isaac Rebirth/mods/<mod>", and any
// path handling that splits on whitespace loses the repository here.
TEST_CASE("git origin is read from a config in a path containing spaces", "[engine]") {
  const fs::path root = make_temp_dir();
  const fs::path mod =
      write_plain_clone(root, "The Binding of Isaac Rebirth/mods/discord-mod", R"([core]
	repositoryformatversion = 0
	filemode = true
	bare = false
	logallrefupdates = true
[remote "origin"]
	url = https://github.com/Alxay/the-binding-of-isaac-rebirth-discord-rich-presence.git
	fetch = +refs/heads/*:refs/remotes/origin/*
[branch "main"]
	remote = origin
	merge = refs/heads/main
	vscode-merge-base = origin/main
)");

  REQUIRE(engine::Git::is_repository(mod));
  CHECK(engine::Git::remote_url(mod) ==
        "https://github.com/Alxay/"
        "the-binding-of-isaac-rebirth-discord-rich-presence.git");
  // The host is what picks the badge, so it is asserted on the same read.
  CHECK(engine::Git::host_of(engine::Git::remote_url(mod)) == "github.com");

  fs::remove_all(root);
}

// Only `origin` is the origin. A second remote's url must never be mistaken
// for it, and `[branch "main"]`'s `remote = origin` key is not a url.
TEST_CASE("git origin wins over a second remote and branch keys", "[engine]") {
  const fs::path root = make_temp_dir();
  const fs::path mod  = write_plain_clone(root, "mod", R"([remote "upstream"]
	url = git@gitlab.com:group/proj.git
	fetch = +refs/heads/*:refs/remotes/upstream/*
[remote "origin"]
	url = https://github.com/user/repo.git
[branch "main"]
	remote = origin
	merge = refs/heads/main
)");

  CHECK(engine::Git::remote_url(mod) == "https://github.com/user/repo.git");

  fs::remove_all(root);
}

// A `[remote "origin"]` written with spaces instead of tabs parses the same -
// git only ever writes tabs, but a hand-edited config must not be the reason a
// mod shows no remote.
TEST_CASE("git origin parses with space indentation too", "[engine]") {
  const fs::path root = make_temp_dir();
  const fs::path mod  = write_plain_clone(root, "mod", R"([remote "origin"]
    url = https://github.com/user/repo.git
)");

  CHECK(engine::Git::remote_url(mod) == "https://github.com/user/repo.git");

  fs::remove_all(root);
}

// A worktree/submodule `.git` FILE points at the real gitdir, and the config
// lives there - so the origin is one indirection further away.
TEST_CASE("git origin is read through a worktree gitdir file", "[engine]") {
  const fs::path root = make_temp_dir();
  const fs::path real = root / "real-git-dir";
  fs::create_directories(real);
  {
    std::ofstream out(real / "config");
    out << "[remote \"origin\"]\n\turl = https://github.com/user/repo.git\n";
  }

  const fs::path mod = root / "mod";
  fs::create_directories(mod);
  {
    std::ofstream out(mod / ".git");
    out << "gitdir: " << real.string() << "\n";
  }

  REQUIRE(engine::Git::is_repository(mod));
  CHECK(engine::Git::remote_url(mod) == "https://github.com/user/repo.git");

  fs::remove_all(root);
}

// No origin configured: an empty string, not another remote's url.
TEST_CASE("a repository with no origin remote has no remote url", "[engine]") {
  const fs::path root = make_temp_dir();
  const fs::path mod  = write_plain_clone(root, "mod", R"([core]
	bare = false
)");

  REQUIRE(engine::Git::is_repository(mod));
  CHECK(engine::Git::remote_url(mod).empty());

  fs::remove_all(root);
}

// Host extraction: what the git badge is picked from. Empty input and
// userinfo-bearing URLs are the two that have bitten before.
TEST_CASE("git host is extracted from every remote url shape", "[engine]") {
  CHECK(engine::Git::host_of("https://github.com/Alxay/repo.git") == "github.com");
  CHECK(engine::Git::host_of("git@github.com:user/repo.git") == "github.com");
  CHECK(engine::Git::host_of("ssh://git@gitlab.com:2222/p.git") == "gitlab.com");
  CHECK(engine::Git::host_of("https://user:tok@git.example.com/p.git") ==
        "git.example.com");
  CHECK(engine::Git::host_of("").empty());
}