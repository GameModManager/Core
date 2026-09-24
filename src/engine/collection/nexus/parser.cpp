#include "engine/collection/nexus/parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace engine::Collection::Nexus {

using json = nlohmann::json;

namespace {

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------

  // Slugify a mod name into a filesystem-safe identifier.
  // "Skyrim 2020 Textures" -> "skyrim-2020-textures"
  std::string slugify(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    bool prev_was_sep = false;
    for (unsigned char c : name) {
      if (std::isalnum(c)) {
        out += static_cast<char>(std::tolower(c));
        prev_was_sep = false;
      } else if (!prev_was_sep && !out.empty()) {
        out += '-';
        prev_was_sep = true;
      }
    }
    // trim trailing separator
    while (!out.empty() && out.back() == '-')
      out.pop_back();
    return out;
  }

  // Extract a required string field from a JSON object, or throw.
  std::string require_string(const json &j, const char *key, const std::string &ctx) {
    if (!j.contains(key) || !j[key].is_string())
      throw ParseError(ctx + ": missing or invalid '" + key + "'");
    return j[key].get<std::string>();
  }

  // Extract an optional string field, defaulting to empty.
  std::string opt_string(const json &j, const char *key) {
    if (j.contains(key) && j[key].is_string())
      return j[key].get<std::string>();
    return {};
  }

  // Extract an optional int64 field, defaulting to def.
  int64_t opt_int64(const json &j, const char *key, int64_t def = 0) {
    if (j.contains(key) && j[key].is_number_integer())
      return j[key].get<int64_t>();
    return def;
  }

  // Extract an optional int field, defaulting to def.
  int opt_int(const json &j, const char *key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer())
      return j[key].get<int>();
    return def;
  }

  // Map a Nexus updatePolicy string to UpdatePolicy enum.
  UpdatePolicy parse_update_policy(const std::string &s) {
    if (s == "latest")
      return UpdatePolicy::Latest;
    return UpdatePolicy::Exact;  // default / "exact"
  }

  // Map the Nexus optional flag to ModCategory.
  ModCategory parse_category(bool optional) {
    return optional ? ModCategory::Optional : ModCategory::Required;
  }

  // Map a modRules type string to RuleType.
  RuleType parse_rule_type(const std::string &s) {
    if (s == "after")
      return RuleType::After;
    if (s == "requires")
      return RuleType::Requires;
    if (s == "conflicts")
      return RuleType::Conflicts;
    return RuleType::Before;  // default
  }

  // ---------------------------------------------------------------------------
  // Parse a single mod entry
  // ---------------------------------------------------------------------------

  ModEntry parse_mod(const json &mod_json, const std::string &domain_override,
                     int index) {
    const std::string ctx = "mods[" + std::to_string(index) + "]";

    ModEntry entry;
    entry.name = require_string(mod_json, "name", ctx);

    // Generate a filesystem-safe id from the mod name.
    entry.id = slugify(entry.name);

    // Version (stored in source, not on ModEntry itself).
    std::string version = opt_string(mod_json, "version");

    // Phase
    entry.phase = opt_int(mod_json, "phase", 0);

    // Category
    bool optional  = mod_json.value("optional", false);
    entry.category = parse_category(optional);

    // Source
    if (!mod_json.contains("source") || !mod_json["source"].is_object())
      throw ParseError(ctx + ": missing 'source' object");

    const auto &src      = mod_json["source"];
    std::string src_type = opt_string(src, "type");

    if (src_type == "nexus" || src_type.empty()) {
      SourceNexus nx;
      nx.resolution  = SourceResolution::Api;
      nx.game_domain = domain_override;
      if (src.contains("gameDomain") && src["gameDomain"].is_string())
        nx.game_domain = src["gameDomain"].get<std::string>();
      nx.mod_id    = opt_int64(src, "modId");
      nx.file_id   = opt_int64(src, "fileId");
      nx.version   = version;
      nx.file_name = opt_string(src, "logicalFilename");
      if (nx.file_name.empty())
        nx.file_name = opt_string(src, "fileExpression");
      nx.file_size = opt_int64(src, "fileSize");
      nx.sha256    = opt_string(src, "md5");  // Nexus uses md5, mapped to sha256 field
      std::string policy_str = opt_string(src, "updatePolicy");
      nx.update_policy       = parse_update_policy(policy_str);
      entry.source           = nx;
    } else if (src_type == "browse" || src_type == "manual") {
      SourceDirect d;
      d.resolution = SourceResolution::Browser;
      d.url        = opt_string(src, "url");
      d.version    = version;
      d.file_name  = opt_string(src, "logicalFilename");
      if (d.file_name.empty())
        d.file_name = opt_string(src, "fileExpression");
      std::string policy_str = opt_string(src, "updatePolicy");
      d.update_policy        = parse_update_policy(policy_str);
      entry.source           = d;
    } else if (src_type == "bundle") {
      // Bundled mods ship inside the collection archive; treat as direct
      // with no URL (download is not needed).
      SourceDirect d;
      d.resolution = SourceResolution::Browser;
      d.url        = opt_string(src, "url");
      d.version    = version;
      d.file_name  = opt_string(src, "logicalFilename");
      if (d.file_name.empty())
        d.file_name = opt_string(src, "fileExpression");
      entry.source = d;
    } else {
      // Unknown source type - default to Direct so we don't lose the entry.
      SourceDirect d;
      d.resolution = SourceResolution::Browser;
      d.version    = version;
      entry.source = d;
    }

    // Installer choices (FOMOD replay)
    if (mod_json.contains("choices") && mod_json["choices"].is_object()) {
      entry.installer_choices.type = "fomod";
      for (auto &[key, val] : mod_json["choices"].items()) {
        if (val.is_array()) {
          std::vector<std::string> opts;
          for (const auto &v : val) {
            if (v.is_string())
              opts.push_back(v.get<std::string>());
          }
          entry.installer_choices.selections[key] = std::move(opts);
        }
      }
    }

    return entry;
  }

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Manifest parse(std::string_view json_str) {
  json root;
  try {
    root = json::parse(json_str);
  } catch (const json::parse_error &e) {
    throw ParseError(std::string("JSON parse error: ") + e.what());
  }

  if (!root.is_object())
    throw ParseError("root is not a JSON object");

  Manifest m;

  // info
  if (root.contains("info") && root["info"].is_object()) {
    const auto &info   = root["info"];
    m.info.name        = opt_string(info, "name");
    m.info.author      = opt_string(info, "author");
    m.info.description = opt_string(info, "description");
    m.info.game_id     = opt_string(info, "domainName");
    m.info.homepage    = opt_string(info, "authorUrl");
  }

  // Schema version from top-level "version" or "schemaVersion" fields.
  m.schema_version = opt_string(root, "version");
  if (m.schema_version.empty())
    m.schema_version = opt_string(root, "schemaVersion");
  if (m.schema_version.empty())
    m.schema_version = "nexus/1";

  // Mods
  if (root.contains("mods") && root["mods"].is_array()) {
    const auto &mods_arr = root["mods"];
    m.mods.reserve(mods_arr.size());
    for (std::size_t i = 0; i < mods_arr.size(); ++i) {
      if (!mods_arr[i].is_object())
        continue;
      m.mods.push_back(parse_mod(mods_arr[i], m.info.game_id, static_cast<int>(i)));
    }
  }

  // Mod rules -> install rules
  if (root.contains("modRules") && root["modRules"].is_array()) {
    const auto &rules_arr = root["modRules"];
    for (std::size_t i = 0; i < rules_arr.size(); ++i) {
      const auto &rj = rules_arr[i];
      if (!rj.is_object())
        continue;

      Rule rule;
      std::string src_mod  = opt_string(rj, "sourceMod");
      std::string tgt_mod  = opt_string(rj, "targetMod");
      std::string type_str = opt_string(rj, "type");

      // sourceMod/targetMod may be numeric mod IDs or names; keep as
      // string for the downstream resolver.
      rule.from = src_mod;
      rule.to   = tgt_mod;
      rule.type = parse_rule_type(type_str);

      if (!rule.from.empty() && !rule.to.empty())
        m.rules.push_back(std::move(rule));
    }
  }

  // Plugin load order hints (game-specific extension, optional)
  if (root.contains("pluginLoadOrder") && root["pluginLoadOrder"].is_array()) {
    for (const auto &v : root["pluginLoadOrder"]) {
      if (v.is_string())
        m.load_order.plugin_hint.push_back(v.get<std::string>());
    }
  }

  return m;
}

Manifest parse_file(const std::string &path) {
  std::ifstream f(path);
  if (!f)
    throw ParseError("cannot open file: " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

}  // namespace engine::Collection::Nexus
