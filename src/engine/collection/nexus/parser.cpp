#include "engine/collection/nexus/parser.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
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

  // Map the Nexus optional flag to ModCategory.
  ModCategory parse_category(bool optional) {
    return optional ? ModCategory::Optional : ModCategory::Required;
  }

  // Map a modRules type string to RuleType. All six values the format emits
  // are recognised; anything else falls back to Before.
  RuleType parse_rule_type(const std::string &s) {
    if (s == "after")
      return RuleType::After;
    if (s == "requires")
      return RuleType::Requires;
    if (s == "conflicts")
      return RuleType::Conflicts;
    if (s == "recommends")
      return RuleType::Recommends;
    if (s == "provides")
      return RuleType::Provides;
    return RuleType::Before;  // default
  }

  // ---------------------------------------------------------------------------
  // Vortex mod reference (modRules[].source / .reference)
  // ---------------------------------------------------------------------------

  // The keys a reference can be resolved by, in the order the format declares
  // them. A reference binds to the first key that matches a mod in the
  // collection; later keys are not consulted once an earlier one has matched.
  struct VortexModReference {
    std::string file_md5;
    int64_t file_size = 0;
    std::string version_match;
    std::string logical_file_name;
    std::string file_expression;
    // Vortex-internal identifiers. The same value is published as a mod's
    // source.tag, so it can be read back, but it is not one of the resolution
    // keys the format defines.
    std::string id_hint;
    std::string md5_hint;
  };

  VortexModReference parse_reference(const json &j) {
    VortexModReference ref;
    if (!j.is_object())
      return ref;
    ref.file_md5          = opt_string(j, "fileMD5");
    ref.file_size         = opt_int64(j, "fileSize");
    ref.version_match     = opt_string(j, "versionMatch");
    ref.logical_file_name = opt_string(j, "logicalFileName");
    ref.file_expression   = opt_string(j, "fileExpression");
    ref.id_hint           = opt_string(j, "idHint");
    ref.md5_hint          = opt_string(j, "md5Hint");
    return ref;
  }

  bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) ==
                    std::tolower(static_cast<unsigned char>(y));
           });
  }

  // Wildcard match over '*' (any run) and '?' (one character), case-blind.
  bool wildcard_match(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
      const char pc = p < pattern.size() ? pattern[p] : '\0';
      if (pc == '?' ||
          (pc != '\0' && std::tolower(static_cast<unsigned char>(pc)) ==
                             std::tolower(static_cast<unsigned char>(text[t])))) {
        ++p;
        ++t;
      } else if (pc == '*') {
        star = p++;
        mark = t;
      } else if (star != std::string_view::npos) {
        p = star + 1;
        t = ++mark;
      } else {
        return false;
      }
    }
    while (p < pattern.size() && pattern[p] == '*')
      ++p;
    return p == pattern.size();
  }

  // "1.2.3" -> {1,2,3}. False for anything else, so a partial version like
  // "2.7" is never silently widened to "2.7.0" and matched against a range.
  bool parse_triple(std::string_view v, int out[3]) {
    std::size_t pos = 0;
    for (int part = 0; part < 3; ++part) {
      if (part > 0) {
        if (pos >= v.size() || v[pos] != '.')
          return false;
        ++pos;
      }
      if (pos >= v.size() || !std::isdigit(static_cast<unsigned char>(v[pos])))
        return false;
      int val = 0;
      while (pos < v.size() && std::isdigit(static_cast<unsigned char>(v[pos]))) {
        val = val * 10 + (v[pos] - '0');
        ++pos;
      }
      out[part] = val;
    }
    return pos == v.size();
  }

  int compare_triple(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; ++i) {
      if (a[i] != b[i])
        return a[i] < b[i] ? -1 : 1;
    }
    return 0;
  }

  // A versionMatch is either an exact version ("1.1.12") or a caret range
  // ("^1.0.0" = >=1.0.0, <2.0.0 - the left-most non-zero part is what may
  // change). Any other syntax matches nothing rather than guessing.
  bool version_matches(std::string_view candidate, std::string_view match) {
    if (match.empty())
      return false;
    if (match.front() != '^')
      return candidate == match;
    int lo[3];
    if (!parse_triple(match.substr(1), lo))
      return false;
    int hi[3] = {lo[0], lo[1], lo[2]};
    if (hi[0] != 0) {
      hi[0] += 1;
      hi[1] = 0;
      hi[2] = 0;
    } else if (hi[1] != 0) {
      hi[1] += 1;
      hi[2] = 0;
    } else {
      hi[2] += 1;
    }
    int got[3];
    if (!parse_triple(candidate, got))
      return false;
    return compare_triple(got, lo) >= 0 && compare_triple(got, hi) < 0;
  }

  // The mod's version, whichever source variant carries it.
  std::string mod_version(const ModEntry &m) {
    return std::visit(
        [](const auto &src) {
          return src.version;
        },
        m.source);
  }

  // The archive file name the mod came from, empty for sources that publish
  // none (the Workshop source has no file name at all).
  std::string mod_file_name(const ModEntry &m) {
    return std::visit(
        [](const auto &src) -> std::string {
          if constexpr (requires { src.file_name; })
            return src.file_name;
          return {};
        },
        m.source);
  }

  // Archive name without its final extension ("skse64_2_02_06.7z" ->
  // "skse64_2_02_06").
  std::string file_stem(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
      return std::string(name);
    return std::string(name.substr(0, dot));
  }

  // First mod matching `pred`, by declaration order.
  // ponytail: no ambiguity reporting - a key that matches two mods (equal file
  // sizes, two mods on one version) binds to the earlier one. Surface the tie
  // if a real collection ever hits it.
  template <typename Pred>
  std::optional<std::string> first_match(const std::vector<ModEntry> &mods, Pred pred) {
    for (const auto &m : mods) {
      if (pred(m))
        return m.id;
    }
    return std::nullopt;
  }

  std::optional<std::string> match_md5(const VortexModReference &ref,
                                       const std::vector<ModEntry> &mods) {
    if (ref.file_md5.empty())
      return std::nullopt;
    return first_match(mods, [&](const ModEntry &m) {
      const auto *nx = std::get_if<SourceNexus>(&m.source);
      return nx && !nx->md5.empty() && iequals(nx->md5, ref.file_md5);
    });
  }

  std::optional<std::string> match_file_size(const VortexModReference &ref,
                                             const std::vector<ModEntry> &mods) {
    if (ref.file_size <= 0)
      return std::nullopt;
    return first_match(mods, [&](const ModEntry &m) {
      const auto *nx = std::get_if<SourceNexus>(&m.source);
      return nx && nx->file_size == ref.file_size;
    });
  }

  std::optional<std::string> match_version(const VortexModReference &ref,
                                           const std::vector<ModEntry> &mods) {
    if (ref.version_match.empty())
      return std::nullopt;
    return first_match(mods, [&](const ModEntry &m) {
      return version_matches(mod_version(m), ref.version_match);
    });
  }

  std::optional<std::string>
  match_logical_file_name(const VortexModReference &ref,
                          const std::vector<ModEntry> &mods) {
    if (ref.logical_file_name.empty())
      return std::nullopt;
    return first_match(mods, [&](const ModEntry &m) {
      const auto &name = mod_file_name(m);
      return !name.empty() && iequals(name, ref.logical_file_name);
    });
  }

  // Either an exact hit on the mod's name, or a wildcard against the archive
  // name without its extension.
  std::optional<std::string> match_file_expression(const VortexModReference &ref,
                                                   const std::vector<ModEntry> &mods) {
    if (ref.file_expression.empty())
      return std::nullopt;
    return first_match(mods, [&](const ModEntry &m) {
      if (iequals(m.name, ref.file_expression))
        return true;
      const auto &name = mod_file_name(m);
      return !name.empty() && wildcard_match(ref.file_expression, file_stem(name));
    });
  }

  // Resolve a reference to the id of the mod it names, trying the keys in the
  // order the format defines and stopping at the first that matches.
  std::optional<std::string> resolve_reference(const VortexModReference &ref,
                                               const std::vector<ModEntry> &mods) {
    if (auto hit = match_md5(ref, mods); hit)
      return hit;
    if (auto hit = match_file_size(ref, mods); hit)
      return hit;
    if (auto hit = match_version(ref, mods); hit)
      return hit;
    if (auto hit = match_logical_file_name(ref, mods); hit)
      return hit;
    return match_file_expression(ref, mods);
  }

  // What the reference asked for, for the unresolved report.
  std::string describe_reference(const VortexModReference &ref) {
    std::string out;
    const auto add = [&out](const char *key, const std::string &value) {
      if (value.empty())
        return;
      if (!out.empty())
        out += ' ';
      out += key;
      out += '=';
      out += value;
    };
    add("fileMD5", ref.file_md5);
    add("fileSize", ref.file_size > 0 ? std::to_string(ref.file_size) : std::string());
    add("versionMatch", ref.version_match);
    add("logicalFileName", ref.logical_file_name);
    add("fileExpression", ref.file_expression);
    return out.empty() ? "reference is empty" : out;
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
      // Nexus publishes md5 of the archive, never a sha256. It stays in the
      // md5 field so a 32-hex digest can never be read back as the 64-hex
      // sha256 an exact pin is defined by.
      nx.md5                 = opt_string(src, "md5");
      std::string policy_str = opt_string(src, "updatePolicy");
      nx.update_policy       = map_update_policy(policy_str);
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
      d.update_policy        = map_update_policy(policy_str);
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

UpdatePolicy map_update_policy(std::string_view nexus_policy) {
  if (nexus_policy == "latest")
    return UpdatePolicy::Latest;
  // "prefer", "exact", absent, and anything unrecognised: keep the pin, allow
  // the fallback. See parser.h for why nothing maps to Exact.
  return UpdatePolicy::Prefer;
}

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

  // Mod rules -> install rules. Both ends of a rule are Vortex references, not
  // names: each is resolved to a mod id here, because rules reference stable
  // ids and never file names. A rule whose ends cannot be resolved is reported
  // rather than dropped.
  if (root.contains("modRules") && root["modRules"].is_array()) {
    const auto &rules_arr = root["modRules"];
    for (std::size_t i = 0; i < rules_arr.size(); ++i) {
      const auto &rj = rules_arr[i];
      if (!rj.is_object())
        continue;

      const VortexModReference src_ref =
          parse_reference(rj.value("source", json::object()));
      const VortexModReference oth_ref =
          parse_reference(rj.value("reference", json::object()));

      const auto from = resolve_reference(src_ref, m.mods);
      const auto to   = resolve_reference(oth_ref, m.mods);

      if (from && to) {
        Rule rule;
        rule.type = parse_rule_type(opt_string(rj, "type"));
        rule.from = *from;
        rule.to   = *to;
        m.rules.push_back(std::move(rule));
        continue;
      }

      const auto report = [&m, i](const char *end, const VortexModReference &ref) {
        Unresolved u;
        u.what   = "modRules[" + std::to_string(i) + "]." + end;
        u.reason = "no mod in this collection matches " + describe_reference(ref);
        m.unresolved.push_back(std::move(u));
      };
      if (!from)
        report("source", src_ref);
      if (!to)
        report("reference", oth_ref);
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
