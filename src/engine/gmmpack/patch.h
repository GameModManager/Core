#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::gmmpack {

// One parsed patches/<id>[-N].json entry. Mirrors patch.schema.json; the
// unpacker ticket owns the shared Gmmpack struct, this is the patch engine's
// input shape so the two can merge without touching each other's files.
struct BinaryPatch {
  std::string mod_id;
  std::optional<int> sequence;  // absent = single non-chained patch
  std::string target_path;      // path within the installed mod
  std::string base_file_sha256;  // lowercase hex, file this step applies to
  std::string payload_base64;   // bsdiff payload
};

// Patches for one (mod, target file), sorted ascending by sequence.
struct PatchChain {
  std::string mod_id;
  std::string target_path;
  std::vector<BinaryPatch> steps;
};

// What the Allow/Do Not Allow consent dialog lists. The engine never shows
// UI: the installer builds a plan, shows it, and calls apply_plan() only on
// Allow. Denied (or empty) plans mean mods install unpatched.
struct PatchPlan {
  std::vector<PatchChain> chains;
  std::vector<std::string> affected_mods;  // sorted unique mod ids
};

// Parse one patch JSON document. `filename` is the archive-relative name
// (e.g. "awesome-mod.json" or "awesome-mod-2.json"); modId/sequence inside
// the document MUST agree with it or the entry is rejected.
bool parse_patch_json(const std::string& json_text, const std::string& filename,
                      BinaryPatch& out, std::string& error);

// Group entries into chains (one per mod+target) in ascending sequence
// order. Rejects gaps (sequences must run 1..N), duplicates, and mixing
// sequenced with unsequenced entries for the same target.
bool group_patches(std::vector<BinaryPatch> entries,
                   std::vector<PatchChain>& out_chains, std::string& error);

PatchPlan build_plan(std::vector<PatchChain> chains);

// Apply one chain to the file at mod_dir/target_path: verify each step's
// base SHA-256, apply in order, write the final result atomically (the file
// on disk is untouched unless every step succeeds). Rejects target paths
// that escape mod_dir.
bool apply_chain(const PatchChain& chain, const std::filesystem::path& mod_dir,
                 std::string& error);

// Apply every chain in the plan. `mod_dirs` maps mod id to its installed
// directory. Stops at the first failure; earlier chains stay applied (the
// installer applies onto a staging dir, so retry is cheap).
bool apply_plan(const PatchPlan& plan,
                const std::unordered_map<std::string, std::filesystem::path>& mod_dirs,
                std::string& error);

}  // namespace engine::gmmpack
