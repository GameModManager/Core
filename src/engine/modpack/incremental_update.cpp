#include "engine/modpack/incremental_update.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "engine/gmmpack/unpacker.h"
#include "engine/modpack/ini_edits.h"

namespace engine::modpack {

namespace {

  // ASCII lowercase copy (avoids pulling fs_utils for one fold).
  std::string ascii_lower(std::string s) {
    for (auto &c : s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }

  // Opaque 8-hex change token over content (change detection, not security).
  std::string hash8(const std::string &s) {
    const size_t h = std::hash<std::string>{}(s);
    char buf[17];
    snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(h & 0xffffffffu));
    return std::string(buf);
  }

  // Pinned identity of one mod source. Only present fields compare: a missing
  // pin means "nothing declared", never "changed".
  struct ModPins {
    bool latest      = false;
    bool has_file_id = false;
    int64_t file_id  = 0;
    std::string version;
    std::string hash;
  };

  ModPins mod_pins(const gmmpack::ModEntry &mod) {
    ModPins pins;
    const auto policy = std::visit(
        [](const auto &s) -> std::string {
          return s.update_policy;
        },
        mod.source);
    pins.latest = (policy == "latest");
    std::visit(
        [&](const auto &s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, gmmpack::ModSourceNexus>) {
            if (s.file_id.has_value()) {
              pins.has_file_id = true;
              pins.file_id     = *s.file_id;
            }
            if (s.version.has_value())
              pins.version = *s.version;
            if (s.sha256.has_value())
              pins.hash = *s.sha256;
          } else if constexpr (std::is_same_v<T, gmmpack::ModSourceSteamWorkshop>) {
            if (s.version.has_value())
              pins.version = *s.version;
          } else {
            if (s.version.has_value())
              pins.version = *s.version;
            if (s.sha256.has_value())
              pins.hash = *s.sha256;
          }
        },
        mod.source);
    return pins;
  }

  // Reason the mod must be reinstalled, or nullopt when pins match.
  std::optional<std::string> pin_change_reason(const gmmpack::ModEntry &mod,
                                               const ResolvedModEntry &entry) {
    const ModPins pins = mod_pins(mod);
    if (pins.latest)
      return std::string("latest re-check");
    if (pins.has_file_id && pins.file_id != entry.actual_file_id) {
      return "fileId " + std::to_string(entry.actual_file_id) + " -> " +
             std::to_string(pins.file_id);
    }
    if (!pins.version.empty() && pins.version != entry.actual_version) {
      const std::string from =
          entry.actual_version.empty() ? "?" : entry.actual_version;
      return "version " + from + " -> " + pins.version;
    }
    if (!pins.hash.empty() && pins.hash != entry.actual_hash) {
      return std::string("hash changed");
    }
    return std::nullopt;
  }

  // "target::tweak" identity of an applied-ini key; hash empty when unknown.
  struct IniKeyParts {
    std::string target;  // already lowered
    std::string tweak;
    std::string hash;  // "" = unknown content
  };

  std::optional<IniKeyParts> split_ini_key(const std::string &key) {
    const size_t first = key.find("::");
    if (first == std::string::npos)
      return std::nullopt;
    const size_t last = key.rfind("::");
    IniKeyParts parts;
    parts.target = key.substr(0, first);
    if (last == first) {
      parts.tweak = key.substr(first + 2);
    } else {
      parts.tweak = key.substr(first + 2, last - first - 2);
      parts.hash  = key.substr(last + 2);
    }
    if (parts.target.empty() || parts.tweak.empty())
      return std::nullopt;
    return parts;
  }

  // mod_id -> separator path ("" = top level) for every mod in the tree.
  void flatten_tree(const std::vector<gmmpack::TreeNode> &nodes,
                    const std::string &parent,
                    std::unordered_map<std::string, std::string> &out) {
    for (const auto &node : nodes) {
      if (const auto *sep = std::get_if<gmmpack::SeparatorNode>(&node.data)) {
        const std::string path = parent.empty() ? sep->name : parent + "/" + sep->name;
        flatten_tree(sep->children, path, out);
      } else if (const auto *mod = std::get_if<gmmpack::ModNode>(&node.data)) {
        out[mod->id] = parent;
      }
    }
  }

  bool qualifies_for_tree(const InstalledPackState &state, const std::string &mod_id) {
    if (state.skip_in_update(mod_id))
      return false;
    return !state.is_tracked(mod_id) || state.follows_pack_tree(mod_id);
  }

  gmmpack::Diagnostic warn(const std::string &path, const std::string &msg) {
    return {gmmpack::Diagnostic::Severity::Warning, path, msg};
  }

}  // namespace

std::string patch_base(const gmmpack::PatchEntry &patch) {
  if (patch.sequence.has_value()) {
    return patch.mod_id + "#" + std::to_string(*patch.sequence);
  }
  return patch.mod_id;
}

std::string patch_key(const gmmpack::PatchEntry &patch) {
  return patch_base(patch) + ":" +
         hash8(patch.target_path + "|" + patch.base_file_sha256 + "|" +
               patch.algorithm + "|" + patch.payload_base64);
}

std::string ini_key(const std::string &target_file, const std::string &tweak_id,
                    const std::string &content) {
  return ascii_lower(target_file) + "::" + tweak_id + "::" + hash8(content);
}

UpdatePlan
diff_update(const gmmpack::Gmmpack &new_pack, const InstalledPackState &state,
            const std::unordered_map<std::string, std::string> &current_ini_text) {
  UpdatePlan plan;
  plan.pack_id      = new_pack.manifest.id;
  plan.new_revision = new_pack.manifest.revision;
  plan.old_revision = state.installed_revision();

  const auto err = [&](const std::string &path, const std::string &msg) {
    plan.diagnostics.push_back({gmmpack::Diagnostic::Severity::Error, path, msg});
  };
  if (!state.has_pack()) {
    err("pack", "no installed pack: use fresh install, not update");
    return plan;
  }
  if (new_pack.manifest.id != state.pack_id()) {
    err("pack", "pack id mismatch: " + new_pack.manifest.id + " != " + state.pack_id());
    return plan;
  }
  if (new_pack.manifest.revision <= state.installed_revision()) {
    err("revision", "revision " + std::to_string(new_pack.manifest.revision) +
                        " is not newer than installed " +
                        std::to_string(state.installed_revision()));
    return plan;
  }

  // -- patches first: mods need the per-mod patch verdict below --
  std::unordered_set<std::string> applied_patches(state.applied_patches().begin(),
                                                  state.applied_patches().end());
  std::unordered_set<std::string> applied_bases;
  for (const auto &key : state.applied_patches()) {
    const size_t cut = key.find(':');
    applied_bases.insert(key.substr(0, cut));
  }
  std::unordered_set<std::string> new_bases;
  for (const auto &patch : new_pack.patches) {
    new_bases.insert(patch_base(patch));
  }
  std::unordered_set<std::string> patch_changed_mods;
  {
    std::unordered_map<std::string, const gmmpack::ModEntry *> new_mods;
    for (const auto &mod : new_pack.mods)
      new_mods[mod.id] = &mod;
    for (const auto &patch : new_pack.patches) {
      if (new_mods.find(patch.mod_id) == new_mods.end())
        continue;
      if (state.skip_in_update(patch.mod_id))
        continue;
      const std::string key  = patch_key(patch);
      const std::string base = patch_base(patch);
      if (applied_patches.count(key) != 0)
        continue;
      PatchChange change;
      change.mod_id    = patch.mod_id;
      change.patch_key = key;
      change.action    = applied_bases.count(base) != 0 ? PatchChange::Action::Changed
                                                        : PatchChange::Action::New;
      plan.patch_changes.push_back(change);
      patch_changed_mods.insert(patch.mod_id);
    }
    for (const auto &key : state.applied_patches()) {
      const size_t cut         = key.find(':');
      const std::string base   = key.substr(0, cut);
      const size_t hash        = base.find('#');
      const std::string mod_id = base.substr(0, hash);
      if (new_mods.find(mod_id) == new_mods.end())
        continue;  // removals cover
      if (new_bases.count(base) != 0)
        continue;
      if (state.skip_in_update(mod_id))
        continue;
      plan.patch_changes.push_back({PatchChange::Action::Removed, mod_id, key});
      patch_changed_mods.insert(mod_id);
    }
    for (const auto &change : plan.patch_changes) {
      if (change.action != PatchChange::Action::Removed) {
        plan.patch_consent_mods.push_back(change.mod_id);
      }
    }
    std::sort(plan.patch_consent_mods.begin(), plan.patch_consent_mods.end());
    plan.patch_consent_mods.erase(
        std::unique(plan.patch_consent_mods.begin(), plan.patch_consent_mods.end()),
        plan.patch_consent_mods.end());
  }

  // -- per-mod diff --
  {
    std::unordered_map<std::string, const gmmpack::ModEntry *> new_mods;
    for (const auto &mod : new_pack.mods)
      new_mods[mod.id] = &mod;
    for (const auto &mod : new_pack.mods) {
      const ResolvedModEntry *entry = state.resolved_mod(mod.id);
      if (entry == nullptr) {
        plan.fresh_installs.push_back(mod.id);
        continue;
      }
      if (state.skip_in_update(mod.id))
        continue;  // manual or user-removed
      if (auto reason = pin_change_reason(mod, *entry)) {
        plan.reinstalls.push_back({mod.id, *reason});
      } else if (patch_changed_mods.count(mod.id) != 0) {
        plan.reinstalls.push_back({mod.id, "patch changed"});
      }
    }
    for (const auto &[mod_id, entry] : state.all_resolved_mods()) {
      if (new_mods.find(mod_id) != new_mods.end())
        continue;
      if (entry.origin == PackModOrigin::Manual)
        continue;  // never pack-owned
      if (entry.presence == PackModPresence::Installed) {
        plan.removals.push_back(mod_id);
      }
    }
    std::sort(plan.removals.begin(), plan.removals.end());
  }

  // -- tree diff: conforming mods follow the new layout, nothing else moves --
  {
    std::unordered_map<std::string, std::string> new_pos;
    flatten_tree(new_pack.tree.nodes, "", new_pos);
    std::unordered_map<std::string, std::string> old_pos;
    bool have_old_tree = false;
    if (!state.tree_snapshot().empty()) {
      try {
        old_pos.clear();
        flatten_tree(
            gmmpack::parse_tree(nlohmann::json::parse(state.tree_snapshot())).nodes, "",
            old_pos);
        have_old_tree = true;
      } catch (const nlohmann::json::exception &) {
        plan.diagnostics.push_back(
            warn("tree.json", "snapshot unparsable: leaving layout alone"));
      }
    } else {
      plan.diagnostics.push_back(
          warn("tree.json", "no snapshot: leaving layout alone"));
    }
    if (have_old_tree) {
      std::unordered_set<std::string> removed(plan.removals.begin(),
                                              plan.removals.end());
      // Pack order keeps widget output stable.
      for (const auto &mod : new_pack.mods) {
        if (!qualifies_for_tree(state, mod.id))
          continue;
        if (removed.count(mod.id) != 0)
          continue;
        const auto new_it = new_pos.find(mod.id);
        if (new_it == new_pos.end())
          continue;  // not in tree: widget default
        const auto old_it = old_pos.find(mod.id);
        if (old_it == old_pos.end()) {
          plan.tree_changes.push_back(
              {TreeChange::Action::Insert, mod.id, new_it->second, ""});
        } else if (old_it->second != new_it->second) {
          plan.tree_changes.push_back(
              {TreeChange::Action::Move, mod.id, new_it->second, old_it->second});
        }
      }
    }
  }

  // -- INI diff: per (targetFile, tweakId) --
  {
    struct NewTweak {
      std::string target_file;  // new pack casing
      gmmpack::IniTweak tweak;
    };
    std::unordered_map<std::string, NewTweak> fresh;  // lower target + \x1f + id
    for (const auto &file : new_pack.ini_edits) {
      for (const auto &tweak : file.tweaks) {
        fresh[ascii_lower(file.target_file) + "\x1f" + tweak.id] = {file.target_file,
                                                                    tweak};
      }
    }
    struct AppliedTweak {
      std::string target;  // lowered
      std::string tweak;
      std::string hash;
    };
    std::vector<AppliedTweak> applied;
    for (const auto &key : state.applied_ini_edits()) {
      if (auto parts = split_ini_key(key)) {
        applied.push_back({parts->target, parts->tweak, parts->hash});
      }
    }
    const auto owner_removed = [&](const std::string &source_mod_id, bool has_source) {
      if (!has_source || source_mod_id.empty())
        return false;
      const ResolvedModEntry *owner = state.resolved_mod(source_mod_id);
      return owner != nullptr && owner->presence == PackModPresence::Removed;
    };
    const auto effective_enabled = [&](const gmmpack::IniTweak &tweak) {
      return state.ini_tweak_enabled(tweak.id).value_or(tweak.enabled);
    };
    std::unordered_set<std::string> matched;  // fresh keys consumed by applied
    for (const auto &old : applied) {
      const std::string id = old.target + "\x1f" + old.tweak;
      const auto it        = fresh.find(id);
      if (it == fresh.end()) {
        IniChange change;
        change.action      = IniChange::Action::Retract;
        change.target_file = old.target;
        change.tweak_id    = old.tweak;
        plan.ini_changes.push_back(change);
        continue;
      }
      matched.insert(id);
      const gmmpack::IniTweak &tweak = it->second.tweak;
      if (owner_removed(tweak.source_mod_id, tweak.has_source_mod_id)) {
        IniChange change;
        change.action        = IniChange::Action::Retract;
        change.target_file   = it->second.target_file;
        change.tweak_id      = tweak.id;
        change.source_mod_id = tweak.source_mod_id;
        plan.ini_changes.push_back(change);
        continue;
      }
      if (!effective_enabled(tweak))
        continue;  // user/author disabled: alone
      const std::string new_hash = hash8(tweak.content);
      if (!old.hash.empty() && old.hash != new_hash) {
        IniChange change;
        change.action        = IniChange::Action::Reapply;
        change.target_file   = it->second.target_file;
        change.tweak_id      = tweak.id;
        change.source_mod_id = tweak.source_mod_id;
        plan.ini_changes.push_back(change);
      }
    }
    std::vector<std::string> fresh_ids;
    fresh_ids.reserve(fresh.size());
    for (const auto &[id, entry] : fresh)
      fresh_ids.push_back(id);
    std::sort(fresh_ids.begin(), fresh_ids.end());
    for (const auto &id : fresh_ids) {
      const NewTweak &entry = fresh.find(id)->second;
      if (matched.count(id) != 0)
        continue;
      const gmmpack::IniTweak &tweak = entry.tweak;
      if (owner_removed(tweak.source_mod_id, tweak.has_source_mod_id)) {
        continue;  // owner gone: nothing to apply
      }
      if (!effective_enabled(tweak)) {
        plan.diagnostics.push_back(
            warn("ini/" + entry.target_file,
                 "tweak " + tweak.id + " disabled, not applied"));
        continue;
      }
      IniChange change;
      change.action        = IniChange::Action::Apply;
      change.target_file   = entry.target_file;
      change.tweak_id      = tweak.id;
      change.source_mod_id = tweak.source_mod_id;
      plan.ini_changes.push_back(change);
    }
    // Flag drifted keys of otherwise-untouched tweaks (needs disk text).
    if (!current_ini_text.empty()) {
      std::unordered_map<std::string, std::string> disk;
      for (const auto &[target, text] : current_ini_text) {
        disk[ascii_lower(target)] = text;
      }
      std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
          disk_cache;  // lower target -> lower "sec\x1fkey" -> value
      const auto disk_edits = [&](const std::string &target)
          -> const std::unordered_map<std::string, std::string> & {
        auto cached = disk_cache.find(target);
        if (cached != disk_cache.end())
          return cached->second;
        std::unordered_map<std::string, std::string> edits;
        const auto text = disk.find(target);
        if (text != disk.end()) {
          if (auto parsed = parse_ini_content(text->second)) {
            for (const auto &edit : *parsed) {
              edits[ascii_lower(edit.section) + "\x1f" + ascii_lower(edit.key)] =
                  edit.value;
            }
          }
        }
        return disk_cache[target] = std::move(edits);
      };
      for (const auto &old : applied) {
        const std::string id = old.target + "\x1f" + old.tweak;
        const auto it        = fresh.find(id);
        if (it == fresh.end())
          continue;  // retract path owns it
        const gmmpack::IniTweak &tweak = it->second.tweak;
        if (!effective_enabled(tweak))
          continue;
        if (!old.hash.empty() && old.hash != hash8(tweak.content)) {
          continue;  // reapply path owns it
        }
        auto parsed = parse_ini_content(tweak.content);
        if (!parsed.has_value())
          continue;  // validated packs never hit this
        const auto &values = disk_edits(old.target);
        for (const auto &edit : *parsed) {
          const auto found =
              values.find(ascii_lower(edit.section) + "\x1f" + ascii_lower(edit.key));
          if (found != values.end() && found->second != edit.value) {
            IniChange change;
            change.action        = IniChange::Action::FlagUserModified;
            change.target_file   = it->second.target_file;
            change.tweak_id      = tweak.id;
            change.source_mod_id = tweak.source_mod_id;
            change.section       = edit.section;
            change.key           = edit.key;
            change.expected      = edit.value;
            change.actual        = found->second;
            plan.ini_changes.push_back(change);
          }
        }
      }
    }
  }

  // -- informational diagnostics: user intent in force for this run --
  {
    std::vector<std::string> diverged, removed;
    for (const auto &[mod_id, entry] : state.all_resolved_mods()) {
      if (entry.placement == PackModPlacement::Diverged &&
          entry.presence == PackModPresence::Installed) {
        diverged.push_back(mod_id);
      }
      if (entry.presence == PackModPresence::Removed)
        removed.push_back(mod_id);
    }
    std::sort(diverged.begin(), diverged.end());
    std::sort(removed.begin(), removed.end());
    for (const auto &mod_id : diverged) {
      plan.diagnostics.push_back(
          warn("mods/" + mod_id, "diverged: position left alone"));
    }
    for (const auto &mod_id : removed) {
      plan.diagnostics.push_back(
          warn("mods/" + mod_id, "removed by user: skipped by update"));
    }
  }

  return plan;
}

}  // namespace engine::modpack
