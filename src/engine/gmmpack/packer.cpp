#include "engine/gmmpack/packer.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <sstream>
#include <type_traits>
#include <unordered_set>

#include "engine/gmmpack/sha256.h"
#include "engine/gmmpack/tree_parser.h"
#include "engine/gmmpack/uuid.h"

namespace engine::gmmpack
{

namespace
{

  // ---------------------------------------------------------------------------
  // Small parsing helpers
  // ---------------------------------------------------------------------------

  int64_t parse_int64(const std::string& s, bool& ok)
  {
    try {
      size_t pos = 0;
      int64_t v  = std::stoll(s, &pos);
      ok         = pos == s.size();
      return v;
    } catch (...) {
      ok = false;
      return 0;
    }
  }

  bool is_numeric_id(const std::string& s)
  {
    if (s.empty())
      return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i == s.size())
      return false;
    for (; i < s.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(s[i])))
        return false;
    }
    return true;
  }

  std::variant<int64_t, std::string> int_or_string_id(const std::string& s)
  {
    bool ok   = false;
    int64_t v = parse_int64(s, ok);
    if (ok)
      return v;
    return s;
  }

  std::string non_empty(const ModMeta& meta, const std::string& section,
                        const std::string& key)
  {
    std::string v = meta.get(section, key);
    return v.empty() ? std::string{} : v;
  }

  // First non-empty value across several (section, key) candidates.
  std::string first_of(const ModMeta& meta,
                       std::initializer_list<std::pair<const char*, const char*>> keys)
  {
    for (const auto& [section, key] : keys) {
      std::string v = meta.get(section, key);
      if (!v.empty())
        return v;
    }
    return {};
  }

  // ---------------------------------------------------------------------------
  // Slug + timestamp + UUID
  // ---------------------------------------------------------------------------

  // Folder -> exported-mod-id map shared by build_mod_entries/build_tree/
  // build_executables so all three agree. Sorted input -> deterministic.
  std::map<std::string, std::string>
  slug_all(const std::unordered_map<std::string, ModTrackingEntry>& entries)
  {
    std::vector<std::string> folders;
    folders.reserve(entries.size());
    for (const auto& [folder, _] : entries)
      folders.push_back(folder);
    std::sort(folders.begin(), folders.end());

    std::map<std::string, std::string> out;
    std::unordered_set<std::string> used;
    for (const auto& folder : folders) {
      std::string slug = mod_slug(folder);
      if (used.count(slug)) {
        int n = 2;
        while (used.count(slug + "-" + std::to_string(n)))
          ++n;
        slug += "-" + std::to_string(n);
      }
      used.insert(slug);
      out[folder] = slug;
    }
    return out;
  }

  std::string utc_now_iso8601()
  {
    auto now       = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
  }

  // ---------------------------------------------------------------------------
  // Tree construction
  // ---------------------------------------------------------------------------

  int32_t sort_pos(int32_t p)
  {
    // Unset (-1) sorts last.
    return p < 0 ? INT32_MAX : p;
  }

}  // namespace

std::string mod_slug(const std::string& folder_name)
{
  std::string slug;
  slug.reserve(folder_name.size());
  bool last_dash = true;  // trims leading dashes
  for (char c : folder_name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      last_dash = false;
    } else if (!last_dash) {
      slug.push_back('-');
      last_dash = true;
    }
  }
  if (!slug.empty() && slug.back() == '-')
    slug.pop_back();
  if (slug.empty())
    slug = "mod";
  return slug;
}

// ---------------------------------------------------------------------------
// resolve_mod_source
// ---------------------------------------------------------------------------
// resolve_mod_source always yields updatePolicy "latest": exact pinning
// needs the download archive's fileSize/sha256, which the installed
// instance does not retain. build_mod_entries upgrades to "exact" only when
// explicitly requested via PackOptions::update_policies (per-folder user
// choice from the export wizard); latest keeps the output schema-valid
// without fabricating hashes.

std::optional<ModSource> resolve_mod_source(const ModMeta& meta,
                                            const std::string& game_id,
                                            uint32_t steam_appid)
{
  const std::string type    = meta.source_type();
  const std::string sid     = meta.source_id();
  const std::string version = meta.version();

  if (type == "nexus") {
    if (sid.empty())
      return std::nullopt;
    bool ok        = false;
    int64_t mod_id = parse_int64(sid, ok);
    if (!ok)
      return std::nullopt;
    ModSourceNexus s;
    s.resolution = meta.get("Nexusmods", "fileid").empty() ? "browser" : "api";
    s.game_domain =
        first_of(meta, {{"Nexusmods", "gameDomain"}, {"Nexusmods", "game_domain"}});
    if (s.game_domain.empty())
      s.game_domain = game_id;
    s.mod_id           = mod_id;
    std::string fileid = meta.get("Nexusmods", "fileid");
    if (fileid.empty())
      fileid = meta.get("Nexusmods", "file_id");
    if (!fileid.empty()) {
      bool fok    = false;
      int64_t fid = parse_int64(fileid, fok);
      if (fok)
        s.file_id = fid;
    }
    if (!version.empty())
      s.version = version;
    std::string fn = first_of(
        meta, {{"General", "installationFile"}, {"General", "installationfile"}});
    if (!fn.empty())
      s.file_name = fn;
    s.update_policy = "latest";
    return ModSource{s};
  }

  if (type == "loverslab") {
    std::string mid =
        first_of(meta, {{"LoversLab", "fileid"}, {"LoversLab", "file_id"}});
    if (mid.empty())
      mid = sid;
    if (mid.empty())
      return std::nullopt;
    ModSourceLoversLab s;
    s.resolution   = "browser";
    s.mod_id       = int_or_string_id(mid);
    s.section_slug = meta.get("LoversLab", "section_slug");
    if (!version.empty())
      s.version = version;
    std::string fn = meta.get("LoversLab", "archive_filename");
    if (!fn.empty())
      s.file_name = fn;
    s.update_policy = "latest";
    return ModSource{s};
  }

  if (type == "modpub") {
    std::string mid = meta.get("ModPub", "mod_id");
    if (mid.empty())
      mid = sid;
    if (mid.empty())
      return std::nullopt;
    ModSourceModPub s;
    s.resolution = "browser";
    s.mod_id     = int_or_string_id(mid);
    if (!version.empty())
      s.version = version;
    s.update_policy = "latest";
    return ModSource{s};
  }

  if (type == "steam" || type == "steam_workshop") {
    if (sid.empty() || !is_numeric_id(sid))
      return std::nullopt;
    bool ok         = false;
    int64_t item_id = parse_int64(sid, ok);
    if (!ok)
      return std::nullopt;
    ModSourceSteamWorkshop s;
    s.resolution       = "client-subscription";
    s.app_id           = static_cast<int64_t>(steam_appid);
    s.workshop_item_id = item_id;
    s.update_policy    = "latest";
    return ModSource{s};
  }

  if (type == "direct") {
    if (sid.empty())
      return std::nullopt;
    ModSourceDirect s;
    s.resolution = "browser";
    s.url        = sid;
    if (!version.empty())
      s.version = version;
    s.update_policy = "latest";
    return ModSource{s};
  }

  return std::nullopt;  // manual + unknown
}

// ---------------------------------------------------------------------------
// build_tree
// ---------------------------------------------------------------------------

TreeRoot build_tree(const InstanceSnapshot& snapshot,
                    const std::filesystem::path& mods_dir)
{
  // Separators: folder names referenced as someone's parent_separator.
  std::unordered_set<std::string> separators;
  for (const auto& [folder, entry] : snapshot.mod_entries) {
    if (!entry.parent_separator.empty())
      separators.insert(entry.parent_separator);
  }

  // Which non-separator folders survive export (resolvable source)?
  std::unordered_set<std::string> exported;
  if (mods_dir.empty()) {
    for (const auto& [folder, _] : snapshot.mod_entries) {
      if (!separators.count(folder))
        exported.insert(folder);
    }
  } else {
    for (const auto& [folder, _] : snapshot.mod_entries) {
      if (separators.count(folder))
        continue;
      ModMeta meta = ModMeta::load(mods_dir, folder);
      if (resolve_mod_source(meta, snapshot.game_id, snapshot.steam_appid))
        exported.insert(folder);
    }
  }
  auto slugs = slug_all(snapshot.mod_entries);

  // Children lookup: parent folder -> member folders, sorted by position.
  std::unordered_map<std::string, std::vector<std::string>> children;
  std::vector<std::string> top_level;
  for (const auto& [folder, entry] : snapshot.mod_entries) {
    if (separators.count(folder))
      continue;  // separators handled below
    if (!exported.count(folder))
      continue;
    if (!entry.parent_separator.empty() && separators.count(entry.parent_separator)) {
      children[entry.parent_separator].push_back(folder);
    } else {
      top_level.push_back(folder);
    }
  }
  auto by_pos = [&](const std::string& a, const std::string& b) {
    int32_t pa = sort_pos(snapshot.mod_entries.at(a).list_position);
    int32_t pb = sort_pos(snapshot.mod_entries.at(b).list_position);
    return pa != pb ? pa < pb : a < b;
  };
  for (auto& [_, v] : children)
    std::sort(v.begin(), v.end(), by_pos);
  std::sort(top_level.begin(), top_level.end(), by_pos);

  // Separator folders sorted by their own position.
  std::vector<std::string> sep_folders(separators.begin(), separators.end());
  std::sort(sep_folders.begin(), sep_folders.end(),
            [&](const std::string& a, const std::string& b) {
              auto it_a  = snapshot.mod_entries.find(a);
              auto it_b  = snapshot.mod_entries.find(b);
              int32_t pa = it_a == snapshot.mod_entries.end()
                               ? INT32_MAX
                               : sort_pos(it_a->second.list_position);
              int32_t pb = it_b == snapshot.mod_entries.end()
                               ? INT32_MAX
                               : sort_pos(it_b->second.list_position);
              return pa != pb ? pa < pb : a < b;
            });

  // Recursive builder: a separator node nests its child mods and any
  // separator whose parent_separator chains through it. Depth-first via
  // explicit parent links; separator-as-child-of-separator is supported
  // through the same children map (keyed on separator folders that are
  // themselves nested).
  std::function<TreeNode(const std::string&)> mod_node =
      [&](const std::string& folder) {
        const auto& e = snapshot.mod_entries.at(folder);
        return TreeNode{ModNode{slugs.at(folder), !e.hidden && !e.disabled}};
      };

  // Nested separators: a separator folder with a parent_separator that is
  // itself a known separator nests inside it; the rest are top-level.
  std::unordered_map<std::string, std::vector<std::string>> sep_children;
  std::vector<std::string> top_seps;
  for (const auto& s : sep_folders) {
    auto it = snapshot.mod_entries.find(s);
    std::string parent =
        it == snapshot.mod_entries.end() ? "" : it->second.parent_separator;
    if (!parent.empty() && separators.count(parent) && parent != s) {
      sep_children[parent].push_back(s);
    } else {
      top_seps.push_back(s);
    }
  }

  std::function<TreeNode(const std::string&)> sep_node =
      [&](const std::string& sep) -> TreeNode {
    SeparatorNode node;
    node.name      = sep;
    auto it        = snapshot.mod_entries.find(sep);
    node.collapsed = it != snapshot.mod_entries.end() && it->second.collapsed;
    node.color = it != snapshot.mod_entries.end() ? it->second.separator_color : "";
    auto mit       = children.find(sep);
    if (mit != children.end()) {
      for (const auto& m : mit->second)
        node.children.push_back(mod_node(m));
    }
    auto sit = sep_children.find(sep);
    if (sit != sep_children.end()) {
      // Merge nested separators into position order with mods by
      // list_position (both lists already sorted; merge here).
      // ponytail: append after mods - order within a separator beyond
      // mod priority is cosmetic.
      for (const auto& s : sit->second)
        node.children.push_back(sep_node(s));
    }
    return TreeNode{node};
  };

  TreeRoot root;
  // Merge top-level mods and separators by list_position.
  struct Item
  {
    int32_t pos = 0;
    std::string name;
    bool is_sep = false;
  };
  std::vector<Item> items;
  for (const auto& m : top_level) {
    items.push_back({sort_pos(snapshot.mod_entries.at(m).list_position), m, false});
  }
  for (const auto& s : top_seps) {
    auto it = snapshot.mod_entries.find(s);
    items.push_back({it == snapshot.mod_entries.end()
                         ? INT32_MAX
                         : sort_pos(it->second.list_position),
                     s, true});
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    return a.pos != b.pos ? a.pos < b.pos : a.name < b.name;
  });
  for (const auto& item : items) {
    root.nodes.push_back(item.is_sep ? sep_node(item.name) : mod_node(item.name));
  }
  return root;
}

// ---------------------------------------------------------------------------
// build_manifest
// ---------------------------------------------------------------------------

Manifest build_manifest(const InstanceSnapshot& snapshot, const PackOptions& options)
{
  Manifest m;
  m.gmmpack_schema = "1.0.0";
  // Stable pack identity: reuse the instance's modpack_id so re-exports of
  // the same instance keep one id; fresh UUID v4 only for instances that
  // have never been assigned one. Revision bumps monotonically from the
  // instance's last exported revision.
  m.id = snapshot.modpack_id.empty() ? engine::generate_uuid_v4() : snapshot.modpack_id;
  m.revision = static_cast<int>(snapshot.modpack_revision + 1);
  m.info.name =
      snapshot.display_name.empty() ? "Exported Modpack" : snapshot.display_name;
  m.info.author      = options.author.empty() ? "Unknown" : options.author;
  m.info.description = options.description;
  m.info.gmm_game_id = snapshot.game_id.empty() ? "unknown" : snapshot.game_id;
  m.info.homepage    = options.homepage;
  m.info.created_at  = utc_now_iso8601();
  m.info.updated_at  = m.info.created_at;
  // Per-instance settings from snapshot (default-profile settings: the
  // three flags come from profiles[0], the strategy from the snapshot).
  auto& prof = snapshot.profiles;
  m.instance_settings.local_saves = !prof.empty() && prof[0].local_saves;
  m.instance_settings.local_settings = !prof.empty() && prof[0].local_settings;
  m.instance_settings.auto_archive_invalidation =
      !prof.empty() && prof[0].auto_archive_invalidation;
  m.instance_settings.deploy_strategy = snapshot.deploy_strategy;
  return m;
}

// ---------------------------------------------------------------------------
// build_mod_entries
// ---------------------------------------------------------------------------

std::vector<ModEntry> build_mod_entries(const InstanceSnapshot& snapshot,
                                        const std::filesystem::path& mods_dir,
                                        const PackOptions& options)
{
  auto slugs = slug_all(snapshot.mod_entries);

  std::unordered_set<std::string> separators;
  for (const auto& [folder, entry] : snapshot.mod_entries) {
    if (!entry.parent_separator.empty())
      separators.insert(entry.parent_separator);
  }

  struct Row
  {
    int32_t pos = 0;
    std::string folder;
  };
  std::vector<Row> rows;
  for (const auto& [folder, entry] : snapshot.mod_entries) {
    if (separators.count(folder))
      continue;
    rows.push_back({sort_pos(entry.list_position), folder});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.pos != b.pos ? a.pos < b.pos : a.folder < b.folder;
  });

  std::vector<ModEntry> out;
  for (const auto& row : rows) {
    ModMeta meta = ModMeta::load(mods_dir, row.folder);
    auto source  = resolve_mod_source(meta, snapshot.game_id, snapshot.steam_appid);
    if (!source)
      continue;  // manual/unknown: not representable, skip
    const auto policy = options.update_policies.find(row.folder);
    if (policy != options.update_policies.end() && policy->second == "exact") {
      // User asked to pin this mod. Steam workshop resolution is
      // client-subscription based, so exact is meaningless there: those
      // sources always stay "latest".
      std::visit(
          [](auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (!std::is_same_v<T, ModSourceSteamWorkshop>)
              s.update_policy = "exact";
          },
          *source);
    }
    ModEntry e;
    e.id       = slugs.at(row.folder);
    e.name     = row.folder;
    e.phase    = 0;
    e.category = ModCategory::Optional;
    e.source   = std::move(*source);
    out.push_back(std::move(e));
  }
  return out;
}

// ---------------------------------------------------------------------------
// build_executables
// ---------------------------------------------------------------------------

std::vector<ExecutableEntry> build_executables(const InstanceSnapshot& snapshot,
                                               const std::filesystem::path& mods_dir)
{
  // Exported mod ids, to validate sourceModId references.
  std::unordered_set<std::string> exported_ids;
  {
    auto slugs = slug_all(snapshot.mod_entries);
    std::unordered_set<std::string> separators;
    for (const auto& [folder, entry] : snapshot.mod_entries) {
      if (!entry.parent_separator.empty())
        separators.insert(entry.parent_separator);
    }
    for (const auto& [folder, _] : snapshot.mod_entries) {
      if (separators.count(folder))
        continue;
      if (mods_dir.empty()) {
        exported_ids.insert(slugs.at(folder));
        continue;
      }
      ModMeta meta = ModMeta::load(mods_dir, folder);
      if (resolve_mod_source(meta, snapshot.game_id, snapshot.steam_appid))
        exported_ids.insert(slugs.at(folder));
    }
  }
  auto slugs = slug_all(snapshot.mod_entries);

  std::vector<ExecutableEntry> out;
  for (const auto& exec : snapshot.executables) {
    if (exec.path.empty())
      continue;
    if (exec.mod.empty())
      continue;  // game-root exe: no source mod
    auto it = slugs.find(exec.mod);
    if (it == slugs.end() || !exported_ids.count(it->second))
      continue;

    ExecutableEntry e;
    std::string base = std::filesystem::path(exec.path).stem().string();
    e.id             = mod_slug(!exec.title.empty() ? exec.title : base);
    e.source_mod_id  = it->second;
    e.relative_path  = exec.path;
    {
      std::istringstream ss(exec.args);
      std::string tok;
      while (ss >> tok)
        e.arguments.push_back(tok);  // ponytail: no quote handling
    }
    for (const auto& kv : exec.env) {
      auto eq = kv.find('=');
      if (eq == std::string::npos || eq == 0)
        continue;
      e.env_vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    e.working_dir = exec.cwd;
    e.role        = "launcher";
    out.push_back(std::move(e));
  }
  return out;
}

// ---------------------------------------------------------------------------
// JSON serializers (reverse of unpacker.cpp parse_*)
// ---------------------------------------------------------------------------

nlohmann::json serialize_mod_source(const ModSource& source)
{
  return std::visit(
      [](const auto& s) -> nlohmann::json {
        using T = std::decay_t<decltype(s)>;
        nlohmann::json j;
        j["provider"]   = s.provider;
        j["resolution"] = s.resolution;
        if constexpr (std::is_same_v<T, ModSourceNexus>) {
          j["gameDomain"] = s.game_domain;
          j["modId"]      = s.mod_id;
          if (s.file_id)
            j["fileId"] = *s.file_id;
          if (s.version)
            j["version"] = *s.version;
          if (s.file_name)
            j["fileName"] = *s.file_name;
          if (s.file_size)
            j["fileSize"] = *s.file_size;
          if (s.sha256)
            j["sha256"] = *s.sha256;
          j["updatePolicy"] = s.update_policy;
        } else if constexpr (std::is_same_v<T, ModSourceLoversLab>) {
          std::visit(
              [&](const auto& id) {
                j["modId"] = id;
              },
              s.mod_id);
          j["sectionSlug"] = s.section_slug;
          if (s.version)
            j["version"] = *s.version;
          if (s.file_name)
            j["fileName"] = *s.file_name;
          if (s.sha256)
            j["sha256"] = *s.sha256;
          j["updatePolicy"] = s.update_policy;
        } else if constexpr (std::is_same_v<T, ModSourceModPub>) {
          std::visit(
              [&](const auto& id) {
                j["modId"] = id;
              },
              s.mod_id);
          if (s.version)
            j["version"] = *s.version;
          if (s.file_name)
            j["fileName"] = *s.file_name;
          if (s.sha256)
            j["sha256"] = *s.sha256;
          j["updatePolicy"] = s.update_policy;
        } else if constexpr (std::is_same_v<T, ModSourceSteamWorkshop>) {
          j["appId"]          = s.app_id;
          j["workshopItemId"] = s.workshop_item_id;
          if (s.version)
            j["version"] = *s.version;
          j["updatePolicy"] = s.update_policy;
        } else if constexpr (std::is_same_v<T, ModSourceDirect>) {
          j["url"] = s.url;
          if (s.version)
            j["version"] = *s.version;
          if (s.file_name)
            j["fileName"] = *s.file_name;
          if (s.sha256)
            j["sha256"] = *s.sha256;
          j["updatePolicy"] = s.update_policy;
        }
        return j;
      },
      source);
}

static std::string category_name(ModCategory c)
{
  switch (c) {
  case ModCategory::Required:
    return "required";
  case ModCategory::Recommended:
    return "recommended";
  case ModCategory::Optional:
  default:
    return "optional";
  }
}

nlohmann::json serialize_mod_entry(const ModEntry& m)
{
  nlohmann::json j;
  j["id"]       = m.id;
  j["name"]     = m.name;
  j["phase"]    = m.phase;
  j["category"] = category_name(m.category);
  j["source"]   = serialize_mod_source(m.source);
  if (m.installer_choices) {
    nlohmann::json ic;
    ic["type"]         = m.installer_choices->type;
    nlohmann::json sel = nlohmann::json::object();
    for (const auto& [k, v] : m.installer_choices->selections)
      sel[k] = v;
    ic["selections"]      = std::move(sel);
    j["installerChoices"] = std::move(ic);
  }
  return j;
}

nlohmann::json serialize_executable_entry(const ExecutableEntry& e)
{
  nlohmann::json j;
  j["id"]           = e.id;
  j["sourceModId"]  = e.source_mod_id;
  j["relativePath"] = e.relative_path;
  j["role"]         = e.role;
  if (!e.arguments.empty())
    j["arguments"] = e.arguments;
  if (!e.env_vars.empty()) {
    nlohmann::json env = nlohmann::json::object();
    for (const auto& [k, v] : e.env_vars)
      env[k] = v;
    j["envVars"] = std::move(env);
  }
  if (!e.working_dir.empty())
    j["workingDir"] = e.working_dir;
  if (e.auto_run)
    j["autoRun"] = e.auto_run;
  if (e.rerun_on_modset_change)
    j["rerunOnModsetChange"] = e.rerun_on_modset_change;
  if (e.requires_virtual_fs_visible)
    j["requiresVirtualFsVisible"] = e.requires_virtual_fs_visible;
  return j;
}

nlohmann::json serialize_manifest(const Manifest& m)
{
  nlohmann::json j;
  j["gmmpackSchema"] = m.gmmpack_schema;
  j["id"]            = m.id;
  j["revision"]      = m.revision;
  nlohmann::json info;
  info["name"]   = m.info.name;
  info["author"] = m.info.author;
  if (!m.info.description.empty())
    info["description"] = m.info.description;
  info["gmmGameId"] = m.info.gmm_game_id;
  if (!m.info.homepage.empty())
    info["homepage"] = m.info.homepage;
  info["createdAt"] = m.info.created_at;
  info["updatedAt"] = m.info.updated_at;
  j["info"]         = std::move(info);
  if (!m.tools.empty()) {
    j["tools"] = nlohmann::json::array();
    for (const auto& t : m.tools) {
      nlohmann::json tj;
      tj["id"]   = t.id;
      tj["name"] = t.name;
      if (!t.homepage.empty())
        tj["homepage"] = t.homepage;
      j["tools"].push_back(std::move(tj));
    }
  }
  auto ser_plat = [](const std::optional<PlatformOverride>& plat) {
    nlohmann::json pj;
    if (!plat->proton_version_pin.empty())
      pj["protonVersionPin"] = plat->proton_version_pin;
    if (plat->steam_overlay)
      pj["steamOverlay"] = *plat->steam_overlay;
    if (!plat->launch_options.empty())
      pj["launchOptions"] = plat->launch_options;
    if (!plat->prefix_files.empty()) {
      pj["prefixFiles"] = nlohmann::json::array();
      for (const auto& pf : plat->prefix_files) {
        pj["prefixFiles"].push_back(
            {{"path", pf.path}, {"sourceModId", pf.source_mod_id}});
      }
    }
    return pj;
  };
  if (m.platform.linux_plat || m.platform.macos || m.platform.windows) {
    nlohmann::json plat = nlohmann::json::object();
    if (m.platform.linux_plat)
      plat["linux"] = ser_plat(m.platform.linux_plat);
    if (m.platform.macos)
      plat["macos"] = ser_plat(m.platform.macos);
    if (m.platform.windows)
      plat["windows"] = ser_plat(m.platform.windows);
    j["platform"] = std::move(plat);
  }
  if (!m.rules.empty()) {
    j["rules"] = nlohmann::json::array();
    for (const auto& r : m.rules) {
      nlohmann::json rj;
      rj["type"] = r.type;
      rj["from"] = r.from;
      rj["to"]   = r.to;
      if (!r.note.empty())
        rj["note"] = r.note;
      j["rules"].push_back(std::move(rj));
    }
  }
  if (!m.load_order.plugin_hint.empty())
    j["loadOrder"] = {{"pluginHint", m.load_order.plugin_hint}};
  if (!m.choice_groups.empty()) {
    j["choiceGroups"] = nlohmann::json::array();
    for (const auto& cg : m.choice_groups) {
      j["choiceGroups"].push_back({{"id", cg.id},
                                   {"name", cg.name},
                                   {"mode", cg.mode},
                                   {"memberModIds", cg.member_mod_ids}});
    }
  }
  nlohmann::json fh = nlohmann::json::object();
  for (const auto& [path, hash] : m.archive.file_hashes)
    fh[path] = hash;
  j["archive"] = {{"fileHashes", std::move(fh)}};
  nlohmann::json is;
  is["localSaves"] = m.instance_settings.local_saves;
  is["localSettings"] = m.instance_settings.local_settings;
  is["automaticArchiveInvalidation"] = m.instance_settings.auto_archive_invalidation;
  if (!m.instance_settings.deploy_strategy.empty())
    is["deployStrategy"] = m.instance_settings.deploy_strategy;
  j["instanceSettings"] = is;
  return j;
}

// ---------------------------------------------------------------------------
// build_gmmpack
// ---------------------------------------------------------------------------

Gmmpack build_gmmpack(const InstanceSnapshot& snapshot,
                      const std::filesystem::path& mods_dir, const PackOptions& options)
{
  Gmmpack pack;
  pack.manifest    = build_manifest(snapshot, options);
  pack.mods        = build_mod_entries(snapshot, mods_dir, options);
  pack.executables = build_executables(snapshot, mods_dir);
  pack.tree        = build_tree(snapshot, mods_dir);
  if (!options.instructions.empty())
    pack.instructions = options.instructions;
  return pack;
}

// ---------------------------------------------------------------------------
// Archive writing
// ---------------------------------------------------------------------------

static bool write_zip_entry(struct archive* a, const std::string& path,
                            const std::string& content, std::string& error)
{
  struct archive_entry* e = archive_entry_new();
  archive_entry_set_pathname(e, path.c_str());
  archive_entry_set_size(e, static_cast<la_int64_t>(content.size()));
  archive_entry_set_filetype(e, AE_IFREG);
  archive_entry_set_perm(e, 0644);
  if (archive_write_header(a, e) != ARCHIVE_OK) {
    error = archive_error_string(a) ? archive_error_string(a)
                                    : "archive_write_header failed";
    archive_entry_free(e);
    return false;
  }
  if (!content.empty() && archive_write_data(a, content.data(), content.size()) < 0) {
    error =
        archive_error_string(a) ? archive_error_string(a) : "archive_write_data failed";
    archive_entry_free(e);
    return false;
  }
  archive_entry_free(e);
  return true;
}

PackResult create_gmmpack(const InstanceSnapshot& snapshot,
                          const std::filesystem::path& mods_dir,
                          const PackOptions& options,
                          const std::filesystem::path& output_path)
{
  PackResult result;
  result.output_path = output_path;

  Gmmpack pack = build_gmmpack(snapshot, mods_dir, options);

  // Serialize every payload first; hash them into the manifest.
  std::vector<std::pair<std::string, std::string>> files;
  for (const auto& mod : pack.mods) {
    files.emplace_back("mods/" + mod.id + ".json", serialize_mod_entry(mod).dump(2));
  }
  for (const auto& exec : pack.executables) {
    files.emplace_back("executables/" + exec.id + ".json",
                       serialize_executable_entry(exec).dump(2));
  }
  files.emplace_back("tree.json", serialize_tree(pack.tree).dump(2));
  if (pack.instructions)
    files.emplace_back("instructions.md", *pack.instructions);

  for (const auto& [path, content] : files) {
    pack.manifest.archive.file_hashes[path] = "sha256:" + sha256_hex(content);
  }
  const std::string manifest_json = serialize_manifest(pack.manifest).dump(2);

  struct archive* a = archive_write_new();
  archive_write_set_format_zip(a);
  archive_write_set_options(a, "zip:compression=deflate");
  if (archive_write_open_filename(a, output_path.string().c_str()) != ARCHIVE_OK) {
    result.error =
        archive_error_string(a) ? archive_error_string(a) : "cannot open output file";
    archive_write_free(a);
    return result;
  }
  std::string error;
  bool ok = write_zip_entry(a, "manifest.json", manifest_json, error);
  for (const auto& [path, content] : files) {
    if (ok)
      ok = write_zip_entry(a, path, content, error);
  }
  if (ok) {
    ok = archive_write_close(a) == ARCHIVE_OK;
    if (!ok)
      error = archive_error_string(a) ? archive_error_string(a)
                                      : "archive_write_close failed";
  }
  archive_write_free(a);

  if (!ok) {
    result.error = error;
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace engine::gmmpack
