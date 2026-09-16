#include "engine/install/conflict_resolver.h"

#include "engine/core/log/logger.h"
#include "engine/core/vfs/path_resolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace engine::Install
{

namespace
{

  // The CI identity key for staged archives and display names. Same single
  // source ConflictEngine's Phase-2 registry uses (FULL variant: separators
  // folded, every component lowercased). Purely lexical - never touches disk,
  // so the resolver is built on an empty root and used for normalize() only.
  const vfs::PathResolver& identity_key()
  {
    static const vfs::PathResolver resolver{std::filesystem::path{},
                                            vfs::NameCompare::CaseInsensitive};
    return resolver;
  }

  [[nodiscard]] std::string lower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  }

  // Provider identity: lowercased source_type + raw source_id. Empty on both
  // sides means "unknown origin" - never a match.
  [[nodiscard]] std::string source_key(const Pack::ResolvedMod& mod)
  {
    if (mod.source_type.empty() && mod.source_id.empty())
      return {};
    return lower(mod.source_type) + '\x1f' + mod.source_id;
  }

  [[nodiscard]] bool same_source(const Pack::ResolvedMod& a, const Pack::ResolvedMod& b)
  {
    const std::string ka = source_key(a);
    return !ka.empty() && ka == source_key(b);
  }

  [[nodiscard]] bool same_file(const Pack::ResolvedMod& a, const Pack::ResolvedMod& b)
  {
    if (a.archive_name.empty() || b.archive_name.empty())
      return false;
    return identity_key().normalize(a.archive_name) ==
           identity_key().normalize(b.archive_name);
  }

  [[nodiscard]] bool same_name(const Pack::ResolvedMod& a, const Pack::ResolvedMod& b)
  {
    if (a.display_name.empty() || b.display_name.empty())
      return false;
    return identity_key().normalize(a.display_name) ==
           identity_key().normalize(b.display_name);
  }

  [[nodiscard]] Action default_for(ConflictType type)
  {
    // Name collisions are diagnostics: both mods install as-is.
    if (type == ConflictType::NameCollision)
      return Action::Keep;
    // Anything sharing identity with an installed mod keeps the installed one
    // unless the user says otherwise.
    return Action::Skip;
  }

  [[nodiscard]] int action_rank(Action action)
  {
    switch (action) {
    case Action::Skip:
      return 3;
    case Action::Replace:
      return 2;
    case Action::Rename:
      return 1;
    case Action::Keep:
      return 0;
    case Action::Ask:
      return -1;
    }
    return -1;
  }

}  // namespace

std::vector<Conflict>
detect_conflicts(const std::vector<Pack::ResolvedMod>& existing_mods,
                 const std::vector<Pack::ResolvedMod>& pack_mods)
{
  std::vector<Conflict> out;
  for (const auto& pack : pack_mods) {
    for (const auto& existing : existing_mods) {
      Conflict hit;
      hit.existing = existing;
      hit.pack     = pack;
      if (same_source(existing, pack)) {
        hit.type = ConflictType::DuplicateSource;
      } else if (same_file(existing, pack)) {
        hit.type = ConflictType::DuplicateFile;
      } else if (same_name(existing, pack)) {
        hit.type = ConflictType::NameCollision;
      } else {
        continue;
      }
      Logger::instance().warn("ConflictResolver: " + describe(hit));
      out.push_back(std::move(hit));
    }
  }
  return out;
}

std::vector<Resolution> resolve_conflicts(const std::vector<Conflict>& conflicts,
                                          const std::vector<UserChoice>& user_choices)
{
  std::vector<Resolution> out;
  out.reserve(conflicts.size());
  for (std::size_t i = 0; i < conflicts.size(); ++i) {
    Resolution r;
    r.conflict_index = i;
    r.pack_entry_id  = conflicts[i].pack.entry_id;
    r.action         = default_for(conflicts[i].type);
    // Last choice for this conflict wins; out-of-range choices are ignored.
    for (const auto& choice : user_choices) {
      if (choice.conflict_index != i)
        continue;
      if (choice.action == Action::Ask) {
        r.action = default_for(conflicts[i].type);
        r.rename_to.clear();
      } else if (choice.action == Action::Rename && choice.rename_to.empty()) {
        r.action = Action::Skip;
        r.rename_to.clear();
      } else {
        r.action = choice.action;
        r.rename_to =
            choice.action == Action::Rename ? choice.rename_to : std::string{};
      }
    }
    if (r.action == Action::Replace) {
      r.remove_existing = conflicts[i].existing.entry_id;
    }
    out.push_back(std::move(r));
  }
  return out;
}

InstallPlan apply_resolutions(const std::vector<Resolution>& resolutions,
                              const InstallPlan& install_plan)
{
  // Strongest action per pack entry: Skip > Replace > Rename > Keep.
  struct Pick
  {
    Action action = Action::Keep;
    std::string rename_to;
  };
  std::unordered_map<std::string, Pick> picks;
  for (const auto& r : resolutions) {
    if (r.action == Action::Ask)
      continue;  // never effective; ignore
    auto& pick = picks[r.pack_entry_id];
    if (action_rank(r.action) > action_rank(pick.action)) {
      pick.action    = r.action;
      pick.rename_to = r.action == Action::Rename ? r.rename_to : std::string{};
    }
  }
  InstallPlan out;
  out.reserve(install_plan.size());
  for (const auto& entry : install_plan) {
    const auto it       = picks.find(entry.mod.entry_id);
    const Action action = it == picks.end() ? Action::Keep : it->second.action;
    if (action == Action::Skip)
      continue;
    if (action == Action::Rename && !it->second.rename_to.empty()) {
      out.push_back({entry.mod, it->second.rename_to});
    } else {
      out.push_back(entry);
    }
  }
  return out;
}

std::string describe(const Conflict& conflict)
{
  const char* kind = "";
  switch (conflict.type) {
  case ConflictType::DuplicateSource:
    kind = "duplicate-source";
    break;
  case ConflictType::DuplicateFile:
    kind = "duplicate-file";
    break;
  case ConflictType::NameCollision:
    kind = "name-collision";
    break;
  }
  return std::string(kind) + " pack='" + conflict.pack.entry_id + "' (" +
         conflict.pack.display_name + ") existing='" + conflict.existing.entry_id +
         "' (" + conflict.existing.display_name + ")";
}

std::string describe(const Resolution& resolution)
{
  const char* verb = "";
  switch (resolution.action) {
  case Action::Skip:
    verb = "skip";
    break;
  case Action::Replace:
    verb = "replace";
    break;
  case Action::Rename:
    verb = "rename";
    break;
  case Action::Keep:
    verb = "keep";
    break;
  case Action::Ask:
    verb = "ask";
    break;
  }
  std::string out = std::string(verb) + " pack='" + resolution.pack_entry_id + "'";
  if (resolution.action == Action::Rename)
    out += " as='" + resolution.rename_to + "'";
  if (resolution.action == Action::Replace) {
    out += " remove-existing='" + resolution.remove_existing + "'";
  }
  return out;
}

}  // namespace engine::Install
