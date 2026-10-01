#include "engine/source/git/git_info.h"

#include <cctype>
#include <fstream>
#include <system_error>

namespace engine::Git {

namespace {

  // Path of the config a `.git` entry points at. A plain clone has a `.git`
  // directory whose config sits inside it; a worktree or submodule has a
  // `.git` FILE holding `gitdir: <path>`, and the config is in that target.
  std::filesystem::path config_path(const std::filesystem::path &dir) {
    const std::filesystem::path dot_git = dir / ".git";
    std::error_code ec;
    if (std::filesystem::is_directory(dot_git, ec))
      return dot_git / "config";
    if (!std::filesystem::is_regular_file(dot_git, ec))
      return {};

    std::ifstream f(dot_git);
    if (!f)
      return {};
    std::string line;
    while (std::getline(f, line)) {
      // Only the leading "gitdir:" directive matters; a malformed or
      // truncated .git file simply resolves to nothing.
      if (line.rfind("gitdir:", 0) != 0)
        continue;
      std::string target = line.substr(7);
      const auto first   = target.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
        return {};
      target = target.substr(first, target.find_last_not_of(" \t\r\n") - first + 1);
      std::filesystem::path git_dir(target);
      if (git_dir.is_relative())
        git_dir = dir / git_dir;
      return git_dir / "config";
    }
    return {};
  }

  std::string to_lower(std::string s) {
    for (auto &c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

}  // namespace

bool is_repository(const std::filesystem::path &dir) {
  if (dir.empty())
    return false;
  std::error_code ec;
  const std::filesystem::path dot_git = dir / ".git";
  return std::filesystem::is_directory(dot_git, ec) ||
         std::filesystem::is_regular_file(dot_git, ec);
}

std::string remote_url(const std::filesystem::path &dir) {
  const auto cfg = config_path(dir);
  if (cfg.empty())
    return {};
  std::ifstream f(cfg);
  if (!f)
    return {};

  // Minimal INI walk: track the current section, take `url` only from
  // [remote "origin"]. A `[remote "upstream"]` url is a different remote and
  // must not be mistaken for origin.
  std::string line;
  bool in_origin = false;
  while (std::getline(f, line)) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
      continue;
    if (line[first] == ';' || line[first] == '#')
      continue;
    if (line[first] == '[') {
      const auto close = line.find(']', first);
      if (close == std::string::npos) {
        in_origin = false;
        continue;
      }
      std::string section = to_lower(line.substr(first + 1, close - first - 1));
      // Strip the quotes git writes around the remote name.
      const auto quote = section.find('"');
      if (quote != std::string::npos) {
        const auto end_quote = section.find('"', quote + 1);
        if (end_quote != std::string::npos)
          section = section.substr(quote + 1, end_quote - quote - 1);
      }
      in_origin = section == "remote origin";
      continue;
    }
    if (!in_origin)
      continue;
    const auto eq = line.find('=', first);
    if (eq == std::string::npos)
      continue;
    const std::string key = to_lower(line.substr(first, eq - first));
    if (key != "url")
      continue;
    const auto value_first = line.find_first_not_of(" \t", eq + 1);
    if (value_first == std::string::npos)
      return {};
    return line.substr(value_first, line.find_last_not_of(" \t\r\n") - value_first + 1);
  }
  return {};
}

std::string host_of(const std::string &url) {
  std::string rest  = url;
  const auto scheme = rest.find("://");
  if (scheme != std::string::npos)
    rest = rest.substr(scheme + 3);

  // A userinfo may itself contain "/", ":" and even "@" (a password can be
  // almost anything), so the authority can only be delimited by taking what
  // comes AFTER the last "@" - never by scanning for the first separator,
  // which would cut "user:tok" out of "https://user:tok@host/repo".
  const auto at         = rest.rfind('@');
  const size_t start    = at != std::string::npos ? at + 1 : 0;
  const auto path_start = rest.find_first_of("/:", start == 0 ? 0 : start);
  if (path_start == std::string::npos)
    return {};
  std::string authority = rest.substr(start, path_start - start);
  if (authority.empty())
    return {};

  // Strip the port, but leave an IPv6 literal's brackets alone.
  if (!authority.empty() && authority.front() == '[') {
    const auto close = authority.find(']');
    if (close != std::string::npos)
      return to_lower(authority.substr(1, close - 1));
  }
  const auto colon = authority.rfind(':');
  if (colon != std::string::npos)
    authority = authority.substr(0, colon);

  return to_lower(authority);
}

}  // namespace engine::Git
