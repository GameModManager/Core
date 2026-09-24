#include "engine/gmmpack/patch.h"

#include "engine/gmmpack/bsdiff.h"
#include "engine/gmmpack/codec.h"
#include "engine/gmmpack/sha256.h"
#include "engine/profile/safe_write_file.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

namespace engine::gmmpack {
namespace {

  bool is_hex_sha256(const std::string &s) {
    if (s.size() != 64)
      return false;
    for (char c : s) {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
        return false;
    }
    for (char c : s) {
      if (std::isupper(static_cast<unsigned char>(c)))
        return false;
    }
    return true;
  }

  // Split "awesome-mod-2.json" into ("awesome-mod", 2); "skyui.json" into
  // ("skyui", 0) where 0 means "no sequence suffix". Returns false when the
  // stem has no usable mod id.
  bool split_filename(const std::string &filename, std::string &mod_id, int &seq) {
    std::string stem = filename;
    auto slash       = stem.find_last_of("/\\");
    if (slash != std::string::npos)
      stem = stem.substr(slash + 1);
    if (stem.size() <= 5 || stem.substr(stem.size() - 5) != ".json")
      return false;
    stem.erase(stem.size() - 5);
    if (stem.empty())
      return false;
    seq       = 0;
    auto dash = stem.find_last_of('-');
    if (dash != std::string::npos) {
      std::string tail = stem.substr(dash + 1);
      if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) {
        seq = std::stoi(tail);
        if (seq < 1)
          return false;
        stem.erase(dash);
        if (stem.empty())
          return false;
      }
    }
    mod_id = stem;
    return true;
  }

  bool read_file(const std::filesystem::path &p, std::vector<uint8_t> &out,
                 std::string &error) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
      error = "cannot open " + p.string();
      return false;
    }
    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    if (size < 0) {
      error = "cannot stat " + p.string();
      return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char *>(out.data()), size)) {
      error = "cannot read " + p.string();
      return false;
    }
    return true;
  }

  // Resolve mod_dir/target_path without letting ".." or absolute paths escape
  // mod_dir. Case/NFC normalization is the OverlayFS layer's job (per the
  // schema); this is purely a traversal guard.
  bool resolve_target(const std::filesystem::path &mod_dir,
                      const std::string &target_path, std::filesystem::path &out,
                      std::string &error) {
    if (target_path.empty()) {
      error = "empty targetPath";
      return false;
    }
    std::filesystem::path rel(target_path);
    if (rel.is_absolute()) {
      error = "absolute targetPath: " + target_path;
      return false;
    }
    std::filesystem::path norm = (mod_dir / rel).lexically_normal();
    std::filesystem::path base = mod_dir.lexically_normal();
    std::string ns             = norm.string();
    std::string bs             = base.string();
    if (ns != bs && (ns.size() <= bs.size() || ns.compare(0, bs.size(), bs) != 0 ||
                     (ns[bs.size()] != '/' && ns[bs.size()] != '\\'))) {
      error = "targetPath escapes mod dir: " + target_path;
      return false;
    }
    if (ns == bs) {
      error = "targetPath is the mod dir itself: " + target_path;
      return false;
    }
    out = norm;
    return true;
  }

}  // namespace

bool parse_patch_json(const std::string &json_text, const std::string &filename,
                      BinaryPatch &out, std::string &error) {
  std::string file_mod;
  int file_seq = 0;
  if (!split_filename(filename, file_mod, file_seq)) {
    error = "bad patch filename (want <modId>[-N].json): " + filename;
    return false;
  }
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::exception &e) {
    error = filename + ": invalid JSON: " + e.what();
    return false;
  }
  if (!doc.is_object()) {
    error = filename + ": patch must be a JSON object";
    return false;
  }
  auto need_str = [&](const char *key, std::string &v) {
    auto it = doc.find(key);
    if (it == doc.end() || !it->is_string() || it->get<std::string>().empty()) {
      error = filename + ": missing/invalid '" + key + "'";
      return false;
    }
    v = it->get<std::string>();
    return true;
  };
  BinaryPatch p;
  if (!need_str("modId", p.mod_id))
    return false;
  if (!need_str("targetPath", p.target_path))
    return false;
  if (!need_str("baseFileSha256", p.base_file_sha256))
    return false;
  std::string algorithm;
  if (!need_str("algorithm", algorithm))
    return false;
  if (!need_str("payloadBase64", p.payload_base64))
    return false;

  if (algorithm != "bsdiff") {
    error = filename + ": unsupported algorithm '" + algorithm + "'";
    return false;
  }
  if (!is_hex_sha256(p.base_file_sha256)) {
    error = filename + ": baseFileSha256 must be lowercase hex sha256";
    return false;
  }
  std::vector<uint8_t> payload;
  if (!base64_decode(p.payload_base64, payload) || payload.empty()) {
    error = filename + ": payloadBase64 is not valid base64";
    return false;
  }
  auto seq_it = doc.find("sequence");
  if (seq_it != doc.end() && !seq_it->is_null()) {
    if (!seq_it->is_number_integer()) {
      error = filename + ": sequence must be an integer";
      return false;
    }
    p.sequence = seq_it->get<int>();
    if (*p.sequence < 1) {
      error = filename + ": sequence must be >= 1";
      return false;
    }
  }
  if (p.mod_id != file_mod) {
    error = filename + ": modId '" + p.mod_id + "' does not match filename";
    return false;
  }
  if (file_seq == 0 && p.sequence.has_value()) {
    error = filename + ": sequence present but filename has no -N suffix";
    return false;
  }
  if (file_seq != 0 && (!p.sequence.has_value() || *p.sequence != file_seq)) {
    error = filename + ": sequence does not match filename suffix";
    return false;
  }
  out = std::move(p);
  return true;
}

bool group_patches(std::vector<BinaryPatch> entries,
                   std::vector<PatchChain> &out_chains, std::string &error) {
  std::map<std::pair<std::string, std::string>, std::vector<BinaryPatch>> groups;
  for (auto &e : entries) {
    groups[{e.mod_id, e.target_path}].push_back(std::move(e));
  }
  out_chains.clear();
  for (auto &[key, steps] : groups) {
    bool any_seq = false, all_seq = true;
    for (auto &s : steps) {
      any_seq = any_seq || s.sequence.has_value();
      all_seq = all_seq && s.sequence.has_value();
    }
    if (any_seq && !all_seq) {
      error = key.first + "/" + key.second +
              ": cannot mix sequenced and unsequenced patches";
      return false;
    }
    if (!any_seq && steps.size() > 1) {
      error = key.first + "/" + key.second +
              ": multiple unsequenced patches for one target";
      return false;
    }
    std::sort(steps.begin(), steps.end(),
              [](const BinaryPatch &a, const BinaryPatch &b) {
                return *a.sequence < *b.sequence;
              });
    for (size_t i = 0; i < steps.size(); ++i) {
      if (steps[i].sequence && *steps[i].sequence != static_cast<int>(i + 1)) {
        error = key.first + "/" + key.second + ": sequence gap (want 1..N)";
        return false;
      }
    }
    PatchChain chain;
    chain.mod_id      = key.first;
    chain.target_path = key.second;
    chain.steps       = std::move(steps);
    out_chains.push_back(std::move(chain));
  }
  std::sort(out_chains.begin(), out_chains.end(),
            [](const PatchChain &a, const PatchChain &b) {
              return std::tie(a.mod_id, a.target_path) <
                     std::tie(b.mod_id, b.target_path);
            });
  return true;
}

PatchPlan build_plan(std::vector<PatchChain> chains) {
  PatchPlan plan;
  plan.chains = std::move(chains);
  for (auto &c : plan.chains) {
    if (std::find(plan.affected_mods.begin(), plan.affected_mods.end(), c.mod_id) ==
        plan.affected_mods.end()) {
      plan.affected_mods.push_back(c.mod_id);
    }
  }
  std::sort(plan.affected_mods.begin(), plan.affected_mods.end());
  return plan;
}

bool apply_chain(const PatchChain &chain, const std::filesystem::path &mod_dir,
                 std::string &error) {
  if (chain.steps.empty()) {
    error = chain.mod_id + ": empty patch chain";
    return false;
  }
  std::filesystem::path target;
  if (!resolve_target(mod_dir, chain.target_path, target, error)) {
    error = chain.mod_id + ": " + error;
    return false;
  }
  std::vector<uint8_t> current;
  if (!read_file(target, current, error)) {
    error = chain.mod_id + "/" + chain.target_path + ": " + error;
    return false;
  }
  for (size_t i = 0; i < chain.steps.size(); ++i) {
    const BinaryPatch &step = chain.steps[i];
    std::string have        = sha256_hex(current);
    if (have != step.base_file_sha256) {
      std::ostringstream msg;
      msg << chain.mod_id << "/" << chain.target_path << " step "
          << (step.sequence.value_or(static_cast<int>(i + 1)))
          << ": base file changed (sha256 mismatch), refusing to patch";
      error = msg.str();
      return false;
    }
    std::vector<uint8_t> payload;
    if (!base64_decode(step.payload_base64, payload)) {
      error = chain.mod_id + ": step payload is not valid base64";
      return false;
    }
    std::vector<uint8_t> next;
    std::string patch_err;
    if (!bsdiff_apply(current.data(), current.size(), payload.data(), payload.size(),
                      next, patch_err)) {
      error = chain.mod_id + "/" + chain.target_path + ": " + patch_err;
      return false;
    }
    current = std::move(next);
  }
  std::string bytes(reinterpret_cast<const char *>(current.data()), current.size());
  if (!engine::profile::safe_write_file(target, bytes)) {
    error = chain.mod_id + "/" + chain.target_path + ": atomic write failed";
    return false;
  }
  return true;
}

bool apply_plan(const PatchPlan &plan,
                const std::unordered_map<std::string, std::filesystem::path> &mod_dirs,
                std::string &error) {
  for (auto &chain : plan.chains) {
    auto it = mod_dirs.find(chain.mod_id);
    if (it == mod_dirs.end()) {
      error = "no installed dir for patched mod '" + chain.mod_id + "'";
      return false;
    }
    if (!apply_chain(chain, it->second, error))
      return false;
  }
  return true;
}

}  // namespace engine::gmmpack
