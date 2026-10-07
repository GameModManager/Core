#include "engine/modpack/model.h"

#include <algorithm>

namespace engine::modpack {

const ModEntry *find_mod(const Gmmpack &pack, const std::string &mod_id) {
  for (const auto &m : pack.mods) {
    if (m.id == mod_id)
      return &m;
  }
  return nullptr;
}

std::optional<std::pair<std::string, int>>
parse_patch_filename(const std::string &path) {
  // Expected: "patches/<mod_id>.json" or "patches/<mod_id>-<N>.json"
  const std::string prefix = "patches/";
  if (path.rfind(prefix, 0) != 0)
    return std::nullopt;

  std::string filename     = path.substr(prefix.size());
  const std::string suffix = ".json";
  if (filename.size() <= suffix.size())
    return std::nullopt;
  if (filename.substr(filename.size() - suffix.size()) != suffix)
    return std::nullopt;

  filename.resize(filename.size() - suffix.size());
  if (filename.empty())
    return std::nullopt;

  // Check for trailing -<N> sequence suffix
  auto dash_pos = filename.rfind('-');
  if (dash_pos != std::string::npos && dash_pos > 0) {
    std::string num_str = filename.substr(dash_pos + 1);
    if (!num_str.empty()) {
      // Verify all digits
      bool all_digits = true;
      for (char c : num_str) {
        if (c < '0' || c > '9') {
          all_digits = false;
          break;
        }
      }
      if (all_digits && !num_str.empty()) {
        int seq = std::stoi(num_str);
        if (seq >= 1) {
          return std::make_pair(filename.substr(0, dash_pos), seq);
        }
      }
    }
  }

  // No valid sequence suffix - single patch
  return std::make_pair(filename, 0);
}

std::pair<std::vector<PatchChain>, Diagnostics>
build_patch_chains(const std::vector<PatchEntry> &patches) {
  std::vector<PatchChain> chains;
  Diagnostics diag;

  // Group by mod_id
  std::unordered_map<std::string, std::vector<const PatchEntry *>> groups;
  for (const auto &p : patches) {
    groups[p.mod_id].push_back(&p);
  }

  for (auto &[mod_id, entries] : groups) {
    // Separate sequenced from non-sequenced
    std::vector<PatchEntry> chained;
    std::vector<PatchEntry> unsequenced;
    for (const auto *pe : entries) {
      if (pe->sequence) {
        chained.push_back(*pe);
      } else {
        unsequenced.push_back(*pe);
      }
    }

    // Sort chained patches by sequence ascending
    std::sort(chained.begin(), chained.end(),
              [](const PatchEntry &a, const PatchEntry &b) {
                return a.sequence.value_or(0) < b.sequence.value_or(0);
              });

    // A mod has either one single patch or a chain, never both
    if (!chained.empty() && !unsequenced.empty()) {
      diag.push_back({Diagnostic::Severity::Error, "patches/" + mod_id,
                      "mod has both a single patch and chained patches"});
    }

    // Validate contiguity for chained patches
    for (size_t i = 0; i < chained.size(); ++i) {
      int expected = static_cast<int>(i + 1);
      if (chained[i].sequence.value_or(0) != expected) {
        diag.push_back({Diagnostic::Severity::Error, "patches/" + mod_id,
                        "non-contiguous sequence: expected " +
                            std::to_string(expected) + ", got " +
                            std::to_string(chained[i].sequence.value_or(0))});
      }
    }

    // Build chain: unsequenced first (single patches), then sorted chained
    PatchChain chain;
    chain.mod_id = mod_id;
    for (auto &p : unsequenced)
      chain.patches.push_back(std::move(p));
    for (auto &p : chained)
      chain.patches.push_back(std::move(p));

    chains.push_back(std::move(chain));
  }

  // Sort chains by mod_id for deterministic output
  std::sort(chains.begin(), chains.end(), [](const PatchChain &a, const PatchChain &b) {
    return a.mod_id < b.mod_id;
  });

  return {std::move(chains), std::move(diag)};
}

}  // namespace engine::modpack