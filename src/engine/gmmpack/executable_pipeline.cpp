#include "engine/gmmpack/executable_pipeline.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <system_error>

#include "engine/core/util/process_utils.h"

namespace engine::gmmpack
{
namespace
{

  namespace fs = std::filesystem;

  const ExecPlatformOverride* platform_override_for(const ExecutableEntry& entry,
                                                    const std::string& os)
  {
    if (os == "linux" && entry.platform.linux_plat.has_value())
      return &entry.platform.linux_plat.value();
    if (os == "macos" && entry.platform.macos.has_value())
      return &entry.platform.macos.value();
    if (os == "windows" && entry.platform.windows.has_value())
      return &entry.platform.windows.value();
    return nullptr;
  }

}  // namespace

std::string host_os_name()
{
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::vector<std::string> merged_environment(const ExecutableEntry& entry,
                                            const std::string& os)
{
  // std::map keeps keys sorted for deterministic output.
  std::map<std::string, std::string> merged(entry.env_vars.begin(),
                                            entry.env_vars.end());
  if (const auto* os_override = platform_override_for(entry, os)) {
    for (const auto& [key, value] : os_override->env_vars)
      merged[key] = value;  // additive: OS wins on conflict
  }
  std::vector<std::string> out;
  out.reserve(merged.size());
  for (const auto& [key, value] : merged)
    out.push_back(key + "=" + value);
  return out;
}

std::vector<ResolvedExecutable> resolve_executables(const Gmmpack& pack,
                                                    const fs::path& mods_root,
                                                    const std::string& os)
{
  std::vector<ResolvedExecutable> out;
  out.reserve(pack.executables.size());
  for (const auto& entry : pack.executables) {
    ResolvedExecutable r;
    r.entry                 = entry;
    const fs::path mod_root = mods_root / entry.source_mod_id;
    r.executable_path       = mod_root / entry.relative_path;
    r.working_dir = entry.working_dir.empty() ? r.executable_path.parent_path()
                                              : mod_root / entry.working_dir;
    if (entry.output.has_value() && !entry.output->path.empty())
      r.output_dir = r.working_dir / entry.output->path;
    r.argv.push_back(r.executable_path.string());
    for (const auto& arg : entry.arguments)
      r.argv.push_back(arg);
    // Output-path injection: pass the resolved absolute output dir when
    // the tool accepts an output-dir flag instead of trusting its default.
    if (entry.output.has_value() && !entry.output->arg_name.empty() &&
        !r.output_dir.empty()) {
      r.argv.push_back(entry.output->arg_name);
      r.argv.push_back(r.output_dir.string());
    }
    r.environment = merged_environment(entry, os);
    out.push_back(std::move(r));
  }
  return out;
}

RunOrder order_for_run(const std::vector<std::string>& ids,
                       const std::vector<ManifestRule>& rules)
{
  const std::set<std::string> known(ids.begin(), ids.end());
  std::map<std::string, std::set<std::string>> edges;  // before -> afters
  std::map<std::string, size_t> position;
  for (size_t i = 0; i < ids.size(); ++i)
    position[ids[i]] = i;

  for (const auto& rule : rules) {
    if (!known.contains(rule.from) || !known.contains(rule.to))
      continue;  // rules may reference plain mods - no edge here
    if (rule.from == rule.to)
      continue;
    if (rule.type == "before") {
      edges[rule.from].insert(rule.to);
    } else if (rule.type == "after" || rule.type == "requires") {
      edges[rule.to].insert(rule.from);
    }
    // "conflicts" and unknown types order nothing.
  }

  // Kahn's algorithm, pack-order tiebreak for determinism.
  std::map<std::string, size_t> indegree;
  for (const auto& id : ids)
    indegree[id] = 0;
  for (const auto& [from, tos] : edges)
    for (const auto& to : tos)
      indegree[to] += 1;

  RunOrder result;
  std::set<std::pair<size_t, std::string>> ready;  // (pack pos, id)
  for (const auto& id : ids)
    if (indegree[id] == 0)
      ready.emplace(position[id], id);
  while (!ready.empty()) {
    // Set ordered by (pack position, id): earliest unconstrained first.
    const std::string id = ready.begin()->second;
    ready.erase(ready.begin());
    result.ids.push_back(id);
    for (const auto& next : edges[id]) {
      if (--indegree[next] == 0)
        ready.emplace(position[next], next);
    }
  }
  result.has_cycle = result.ids.size() != ids.size();
  if (result.has_cycle) {
    // Append the cyclic remainder in pack order - deterministic fallback.
    const std::set<std::string> emitted(result.ids.begin(), result.ids.end());
    for (const auto& id : ids)
      if (!emitted.contains(id))
        result.ids.push_back(id);
  }
  return result;
}

std::vector<std::string> setup_auto_run_ids(const Gmmpack& pack, const RunOrder& order)
{
  std::map<std::string, const ExecutableEntry*> by_id;
  for (const auto& e : pack.executables)
    by_id[e.id] = &e;
  std::vector<std::string> out;
  for (const auto& id : order.ids) {
    const auto it = by_id.find(id);
    if (it == by_id.end())
      continue;  // order may include plain mod ids
    const auto* e = it->second;
    if (e->role == "setup" && e->auto_run)
      out.push_back(id);
  }
  return out;
}

std::string modset_input_hash(const std::vector<std::string>& inputs)
{
  // FNV-1a 64-bit: stable across runs/platforms, no crypto dependency.
  // Change detection only - never used for integrity or security.
  std::vector<std::string> sorted = inputs;
  std::sort(sorted.begin(), sorted.end());
  uint64_t hash = 14695981039346656037ULL;
  for (const auto& token : sorted) {
    for (const unsigned char c : token) {
      hash ^= c;
      hash *= 1099511628211ULL;
    }
    hash ^= 0xFF;  // token separator
    hash *= 1099511628211ULL;
  }
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(16);
  for (int i = 0; i < 8; ++i) {
    const unsigned char byte = static_cast<unsigned char>((hash >> (i * 8)) & 0xFF);
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0F]);
  }
  return out;
}

bool exe_rerun_due(const ExecutableEntry& entry, const std::string& current_hash,
                   const std::string& last_hash)
{
  return entry.rerun_on_modset_change && !last_hash.empty() &&
         current_hash != last_hash;
}

CapturedProcess run_executable(const ResolvedExecutable& resolved)
{
  RunOptions options;
  options.env = resolved.environment;
  options.cwd = resolved.working_dir.string();
#ifndef _WIN32
  // Mod archives rarely preserve Unix permission bits, so a tool script
  // may arrive without +x. Same best-effort treatment as the file-type
  // dispatcher's ensure_executable: failure just falls through to exec.
  std::error_code ec;
  const auto perms = fs::status(resolved.executable_path, ec).permissions();
  if (!ec && (perms & fs::perms::owner_exec) == fs::perms::none) {
    fs::permissions(resolved.executable_path,
                    perms | fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    ec);
  }
#endif
  return run_captured(resolved.argv, options);
}

CaptureResult capture_exe_output(const ResolvedExecutable& resolved,
                                 const fs::path& mods_root)
{
  const bool synthetic = resolved.entry.output.has_value() &&
                         resolved.entry.output->capture == "syntheticMod";
  if (!synthetic || resolved.output_dir.empty())
    return {true, {}};
  const std::string& mod_id = resolved.entry.output->synthetic_mod_id;
  if (mod_id.empty())
    return {false, "syntheticMod capture without syntheticModId"};
  if (mod_id.find('/') != std::string::npos || mod_id.find('\\') != std::string::npos ||
      mod_id == "." || mod_id == "..")
    return {false, "syntheticModId is not a plain folder name: " + mod_id};

  std::error_code ec;
  const fs::path target = mods_root / mod_id;
  fs::remove_all(target, ec);  // derived output: fully replaced, not merged
  if (ec)
    return {false, "cannot clear " + target.string() + ": " + ec.message()};
  if (!fs::exists(resolved.output_dir, ec))
    return {false, "output dir missing: " + resolved.output_dir.string()};
  fs::create_directories(target, ec);
  if (ec)
    return {false, "cannot create " + target.string() + ": " + ec.message()};
  for (fs::recursive_directory_iterator it(resolved.output_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    const fs::path rel = fs::relative(it->path(), resolved.output_dir, ec);
    if (ec)
      break;
    const fs::path dest = target / rel;
    if (it->is_directory(ec)) {
      fs::create_directories(dest, ec);
    } else if (it->is_regular_file(ec)) {
      fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
    }
    if (ec)
      break;
  }
  if (ec)
    return {false, "copy failed: " + std::string(ec.message())};
  return {true, {}};
}

LaunchParams to_launch_params(const ResolvedExecutable& resolved)
{
  LaunchParams params;
  params.executable = resolved.executable_path;
  params.args.assign(resolved.argv.begin() + 1, resolved.argv.end());
  params.environment = resolved.environment;
  params.cwd         = resolved.working_dir;
  return params;
}

}  // namespace engine::gmmpack
