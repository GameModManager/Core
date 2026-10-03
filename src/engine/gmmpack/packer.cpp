#include "engine/gmmpack/packer.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <type_traits>
#include <unordered_set>

#include "engine/gmmpack/bsdiff.h"
#include "engine/gmmpack/codec.h"
#include "engine/gmmpack/ini_edit_parser.h"
#include "engine/gmmpack/sha256.h"
#include "engine/gmmpack/tree_parser.h"
#include "engine/gmmpack/unpacker.h"
#include "engine/gmmpack/uuid.h"
#include "engine/modpack/ini_edits.h"

namespace engine::gmmpack {

namespace {

  // ---------------------------------------------------------------------------
  // Small parsing helpers
  // ---------------------------------------------------------------------------

  int64_t parse_int64(const std::string &s, bool &ok) {
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

  bool is_numeric_id(const std::string &s) {
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

  std::variant<int64_t, std::string> int_or_string_id(const std::string &s) {
    bool ok   = false;
    int64_t v = parse_int64(s, ok);
    if (ok)
      return v;
    return s;
  }

  std::string non_empty(const ModMeta &meta, const std::string &section,
                        const std::string &key) {
    std::string v = meta.get(section, key);
    return v.empty() ? std::string{} : v;
  }

  // First non-empty value across several (section, key) candidates.
  std::string
  first_of(const ModMeta &meta,
           std::initializer_list<std::pair<const char *, const char *>> keys) {
    for (const auto &[section, key] : keys) {
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
  slug_all(const std::unordered_map<std::string, ModTrackingEntry> &entries) {
    std::vector<std::string> folders;
    folders.reserve(entries.size());
    for (const auto &[folder, _] : entries)
      folders.push_back(folder);
    std::sort(folders.begin(), folders.end());

    std::map<std::string, std::string> out;
    std::unordered_set<std::string> used;
    for (const auto &folder : folders) {
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

  std::string utc_now_iso8601() {
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

  int32_t sort_pos(int32_t p) {
    // Unset (-1) sorts last.
    return p < 0 ? INT32_MAX : p;
  }

  // Coarse install-sequencing bucket. Deliberately NOT the mod's list
  // position - tree.json already carries display order, and the schema calls
  // phase "NOT display order". A required mod has to be on disk before an
  // optional add-on that depends on it, which is exactly the sequencing the
  // install widget walks phase by phase.
  int phase_for(ModCategory category) {
    switch (category) {
    case ModCategory::Required:
      return 0;
    case ModCategory::Recommended:
      return 1;
    case ModCategory::Optional:
    default:
      return 2;
    }
  }

  ModCategory category_for(const std::string &name) {
    if (name == "optional")
      return ModCategory::Optional;
    if (name == "recommended")
      return ModCategory::Recommended;
    // Unlabelled or unrecognised = required. An exported pack exists to
    // reproduce the instance it was made from, and every mod in it was put
    // there on purpose, so "the author never said" cannot mean "the importer
    // may drop it".
    return ModCategory::Required;
  }

  // A path relative to a mod folder, always with forward slashes (the pack
  // format's own separator, whatever the exporting platform uses).
  std::string relative_path(const std::filesystem::path &base,
                            const std::filesystem::path &file) {
    std::error_code ec;
    auto rel = std::filesystem::relative(file, base, ec);
    if (ec)
      return {};
    std::string out = rel.generic_string();
    if (out.empty() || out.front() == '.')
      return {};
    return out;
  }

  // Archive path of a payload file: files/<root>/<rel>.
  std::string root_prefix(const ModSourceEmbedded &src) {
    return "files/" + src.root + "/";
  }

  // Every regular file under root, relative and sorted for a deterministic
  // archive (and a deterministic hash list).
  std::vector<std::string> scan_files(const std::filesystem::path &root) {
    std::vector<std::string> out;
    if (root.empty() || !std::filesystem::is_directory(root))
      return out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec)
        break;
      if (!it->is_regular_file(ec))
        continue;
      auto rel = relative_path(root, it->path());
      if (!rel.empty())
        out.push_back(std::move(rel));
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  bool read_file(const std::filesystem::path &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
      return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
  }

  // True once the caller has raised the cancel flag. Every loop that walks a
  // mod folder tests this per file, so an abandoned build stops at the next
  // boundary instead of running to completion.
  bool pack_cancelled(const PackCancel *cancel) {
    return cancel != nullptr && cancel->cancelled();
  }

  // ---------------------------------------------------------------------------
  // Progress reporting
  // ---------------------------------------------------------------------------
  //
  // Every call is a no-op without a progress sink, and each one publishes a real
  // count: the stage's own unit (a mod, a colliding file) for the bar, and the
  // files inside the current unit for the "what is it doing right now" line.
  // Nothing here invents motion - a stage that cannot count itself reports
  // total 0 and the bar holds its boundary until the next stage hands over.

  void report_stage(const PackCancel *cancel, PackStage stage, int total,
                    const std::string &name = {}) {
    if (cancel == nullptr || cancel->progress == nullptr)
      return;
    PackProgress &p = *cancel->progress;
    p.stage.store(static_cast<int>(stage), std::memory_order_relaxed);
    p.total.store(total, std::memory_order_relaxed);
    p.item.store(0, std::memory_order_relaxed);
    p.detail_item.store(0, std::memory_order_relaxed);
    p.detail_total.store(0, std::memory_order_relaxed);
    const std::lock_guard lock(p.name_mutex);
    p.name = name;
  }

  // One unit of the current stage finished. The unit is also what the detail
  // line names, so switching to it clears the previous unit's file counts.
  void report_step(const PackCancel *cancel, const std::string &name) {
    if (cancel == nullptr || cancel->progress == nullptr)
      return;
    PackProgress &p = *cancel->progress;
    p.item.fetch_add(1, std::memory_order_relaxed);
    p.detail_item.store(0, std::memory_order_relaxed);
    p.detail_total.store(0, std::memory_order_relaxed);
    const std::lock_guard lock(p.name_mutex);
    p.name = name;
  }

  // The current unit's own file count, then one file at a time. This is what
  // moves while a single 5 MB texture is being hashed inside one stage unit.
  void report_detail(const PackCancel *cancel, int files) {
    if (cancel == nullptr || cancel->progress == nullptr)
      return;
    PackProgress &p = *cancel->progress;
    p.detail_total.store(files, std::memory_order_relaxed);
    p.detail_item.store(0, std::memory_order_relaxed);
  }

  void report_detail_step(const PackCancel *cancel) {
    if (cancel == nullptr || cancel->progress == nullptr)
      return;
    cancel->progress->detail_item.fetch_add(1, std::memory_order_relaxed);
  }

  // GMM's own bookkeeping at a mod's root (meta.ini, metadata.xml) is manager
  // state, never mod content: it must not become a patch target or an INI
  // tweak. The same file name deeper in the tree (data/foo/meta.ini) is real
  // content and stays.
  bool is_manager_metadata(const std::string &relative) {
    if (relative.find('/') != std::string::npos)
      return false;
    return relative == "meta.ini" || relative == "metadata.xml";
  }

  bool is_hex64(const std::string &s) {
    if (s.size() != 64)
      return false;
    for (char c : s) {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
        return false;
    }
    return true;
  }

  std::string lower(std::string s) {
    for (char &c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  // One-shot setup generators: they read the deployed mod set and write
  // output, which is what role=setup exists for. Matched on the file name so
  // a mod folder can ship the tool under any path.
  // ponytail: a name list, not content sniffing. A mod shipping a tool GMM
  // does not know stays a launcher - add the name here when it shows up.
  bool setup_executable_name(const std::string &basename_lower) {
    static const char *kSetupNames[] = {
        "nemesis.exe", "nemesis",          "fnisis.exe", "fnisis",
        "fnispatch",   "pandora.exe",      "pandora",    "bodyslide.exe",
        "bodyslide",   "synthesis.exe",    "synthesis",  "zedit.exe",
        "zedit",       "ssedit.exe",       "ssedit",     "mutagen.exe",
        "mutagen",     "dyndolved.exe",    "dyndolved",  "addresslibrary.exe",
        "mo2.exe",     "modorganizer.exe",
    };
    for (const char *name : kSetupNames) {
      if (basename_lower == name)
        return true;
    }
    return false;
  }

}  // namespace

std::string mod_slug(const std::string &folder_name) {
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
// resolve_mod_source itself always yields updatePolicy "latest"; it only maps
// a meta.ini to a provider. Upgrading a mod to "exact" is
// build_mod_entries' job, and it does so only when the download's identity
// (fileSize/sha256, plus fileId/version on Nexus) can actually be resolved -
// from the mod's recorded install metadata or from the archive in the
// instance's downloads folder. A pin the pack cannot back up is a pack GMM
// refuses to import, so an unresolved one degrades to "latest" instead.

std::optional<ModSource> resolve_mod_source(const ModMeta &meta,
                                            const std::string &game_id,
                                            uint32_t steam_appid) {
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
// Embedded sources
// ---------------------------------------------------------------------------

bool can_embed_folder(const std::filesystem::path &mods_dir,
                      const std::string &folder) {
  if (mods_dir.empty() || folder.empty())
    return false;
  // Directory walk only: resolve_embedded_source hashes every file, which is
  // not something to do once per row while the Mods page is being built.
  return !scan_files(mods_dir / folder).empty();
}

std::optional<ModSource> resolve_embedded_source(const std::filesystem::path &mods_dir,
                                                 const std::string &folder,
                                                 const PackCancel *cancel) {
  if (!can_embed_folder(mods_dir, folder))
    return std::nullopt;
  const std::filesystem::path root = mods_dir / folder;
  const auto files                 = scan_files(root);
  if (files.empty())
    return std::nullopt;

  ModSourceEmbedded s;
  s.root = mod_slug(folder);
  report_detail(cancel, static_cast<int>(files.size()));
  std::string content;
  for (const auto &rel : files) {
    if (pack_cancelled(cancel))
      return std::nullopt;
    report_detail_step(cancel);
    if (!read_file(root / rel, content))
      continue;
    ModSourceEmbedded::File f;
    f.path   = rel;
    f.size   = static_cast<int64_t>(content.size());
    f.sha256 = sha256_hex(content);
    s.files.push_back(std::move(f));
  }
  if (s.files.empty())
    return std::nullopt;
  return ModSource{s};
}

std::optional<ModSource>
resolve_for_export(const ModMeta &meta, const std::filesystem::path &mods_dir,
                   const std::string &folder, const std::string &game_id,
                   uint32_t steam_appid, const PackOptions &options,
                   const PackCancel *cancel) {
  if (options.embed_folders.count(folder)) {
    if (auto embedded = resolve_embedded_source(mods_dir, folder, cancel))
      return embedded;
  }
  return resolve_mod_source(meta, game_id, steam_appid);
}

// ---------------------------------------------------------------------------
// Download identity (what an "exact" pin needs)
// ---------------------------------------------------------------------------

DownloadIdentity read_download_identity(const ModMeta &meta) {
  DownloadIdentity id;
  // InstallStage stamps these under the provider's own section on every
  // download install, so an exact pin is backed by the bytes that were
  // actually fetched.
  static const char *kSections[] = {"GameModManager", "Nexusmods", "General"};
  id.version                     = meta.version();
  id.file_name                   = first_of(
      meta, {{"General", "installationFile"}, {"General", "installationfile"}});
  for (const char *section : kSections) {
    if (id.file_size == 0) {
      const std::string size = first_of(
          meta,
          {{section, "download_size"}, {section, "filesize"}, {section, "fileSize"}});
      if (!size.empty()) {
        bool ok        = false;
        int64_t parsed = parse_int64(size, ok);
        if (ok)
          id.file_size = parsed;
      }
    }
    if (id.sha256.empty()) {
      const std::string hash =
          first_of(meta, {{section, "download_sha256"}, {section, "sha256"}});
      if (is_hex64(hash))
        id.sha256 = lower(hash);
    }
  }
  return id;
}

DownloadIdentity resolve_download_identity(const ModMeta &meta,
                                           const std::filesystem::path &downloads_dir) {
  DownloadIdentity id = read_download_identity(meta);
  if (id.file_size > 0 && !id.sha256.empty())
    return id;
  // Installed before the identity was recorded: the archive is usually still
  // in the instance's downloads folder, so hash it now.
  if (!id.file_name.empty() && !downloads_dir.empty()) {
    std::string content;
    const std::filesystem::path archive = downloads_dir / id.file_name;
    if (read_file(archive, content) && !content.empty()) {
      id.file_size = static_cast<int64_t>(content.size());
      id.sha256    = sha256_hex(content);
    }
  }
  return id;
}

// ---------------------------------------------------------------------------
// build_tree
// ---------------------------------------------------------------------------

TreeRoot build_tree(const InstanceSnapshot &snapshot,
                    const std::filesystem::path &mods_dir, const PackCancel *cancel) {
  PackOptions options;
  return build_tree(snapshot, mods_dir, options, cancel);
}

TreeRoot build_tree(const InstanceSnapshot &snapshot,
                    const std::filesystem::path &mods_dir, const PackOptions &options,
                    const PackCancel *cancel) {
  // Separators: folder names referenced as someone's parent_separator.
  std::unordered_set<std::string> separators;
  for (const auto &[folder, entry] : snapshot.mod_entries) {
    if (!entry.parent_separator.empty())
      separators.insert(entry.parent_separator);
  }

  // Which non-separator folders survive export (resolvable source)?
  std::unordered_set<std::string> exported;
  const int resolvable =
      static_cast<int>(snapshot.mod_entries.size() - separators.size());
  report_stage(cancel, PackStage::Tree, resolvable);
  if (mods_dir.empty()) {
    for (const auto &[folder, _] : snapshot.mod_entries) {
      if (!separators.count(folder))
        exported.insert(folder);
    }
  } else {
    for (const auto &[folder, _] : snapshot.mod_entries) {
      if (separators.count(folder))
        continue;
      if (pack_cancelled(cancel))
        break;
      report_step(cancel, folder);
      ModMeta meta = ModMeta::load(mods_dir, folder);
      if (resolve_for_export(meta, mods_dir, folder, snapshot.game_id,
                             snapshot.steam_appid, options, cancel))
        exported.insert(folder);
    }
  }
  auto slugs = slug_all(snapshot.mod_entries);

  // Children lookup: parent folder -> member folders, sorted by position.
  std::unordered_map<std::string, std::vector<std::string>> children;
  std::vector<std::string> top_level;
  for (const auto &[folder, entry] : snapshot.mod_entries) {
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
  auto by_pos = [&](const std::string &a, const std::string &b) {
    int32_t pa = sort_pos(snapshot.mod_entries.at(a).list_position);
    int32_t pb = sort_pos(snapshot.mod_entries.at(b).list_position);
    return pa != pb ? pa < pb : a < b;
  };
  for (auto &[_, v] : children)
    std::sort(v.begin(), v.end(), by_pos);
  std::sort(top_level.begin(), top_level.end(), by_pos);

  // Separator folders sorted by their own position.
  std::vector<std::string> sep_folders(separators.begin(), separators.end());
  std::sort(sep_folders.begin(), sep_folders.end(),
            [&](const std::string &a, const std::string &b) {
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
  std::function<TreeNode(const std::string &)> mod_node =
      [&](const std::string &folder) {
        const auto &e = snapshot.mod_entries.at(folder);
        return TreeNode{ModNode{slugs.at(folder), !e.hidden && !e.disabled}};
      };

  // Nested separators: a separator folder with a parent_separator that is
  // itself a known separator nests inside it; the rest are top-level.
  std::unordered_map<std::string, std::vector<std::string>> sep_children;
  std::vector<std::string> top_seps;
  for (const auto &s : sep_folders) {
    auto it = snapshot.mod_entries.find(s);
    std::string parent =
        it == snapshot.mod_entries.end() ? "" : it->second.parent_separator;
    if (!parent.empty() && separators.count(parent) && parent != s) {
      sep_children[parent].push_back(s);
    } else {
      top_seps.push_back(s);
    }
  }

  std::function<TreeNode(const std::string &)> sep_node =
      [&](const std::string &sep) -> TreeNode {
    SeparatorNode node;
    node.name      = sep;
    auto it        = snapshot.mod_entries.find(sep);
    node.collapsed = it != snapshot.mod_entries.end() && it->second.collapsed;
    node.color     = it != snapshot.mod_entries.end() ? it->second.separator_color : "";
    auto mit       = children.find(sep);
    if (mit != children.end()) {
      for (const auto &m : mit->second)
        node.children.push_back(mod_node(m));
    }
    auto sit = sep_children.find(sep);
    if (sit != sep_children.end()) {
      // Merge nested separators into position order with mods by
      // list_position (both lists already sorted; merge here).
      // ponytail: append after mods - order within a separator beyond
      // mod priority is cosmetic.
      for (const auto &s : sit->second)
        node.children.push_back(sep_node(s));
    }
    return TreeNode{node};
  };

  TreeRoot root;
  // Merge top-level mods and separators by list_position.
  struct Item {
    int32_t pos = 0;
    std::string name;
    bool is_sep = false;
  };
  std::vector<Item> items;
  for (const auto &m : top_level) {
    items.push_back({sort_pos(snapshot.mod_entries.at(m).list_position), m, false});
  }
  for (const auto &s : top_seps) {
    auto it = snapshot.mod_entries.find(s);
    items.push_back({it == snapshot.mod_entries.end()
                         ? INT32_MAX
                         : sort_pos(it->second.list_position),
                     s, true});
  }
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
    return a.pos != b.pos ? a.pos < b.pos : a.name < b.name;
  });
  for (const auto &item : items) {
    root.nodes.push_back(item.is_sep ? sep_node(item.name) : mod_node(item.name));
  }
  return root;
}

// ---------------------------------------------------------------------------
// build_manifest
// ---------------------------------------------------------------------------

Manifest build_manifest(const InstanceSnapshot &snapshot, const PackOptions &options) {
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
  auto &prof                         = snapshot.profiles;
  m.instance_settings.local_saves    = !prof.empty() && prof[0].local_saves;
  m.instance_settings.local_settings = !prof.empty() && prof[0].local_settings;
  m.instance_settings.auto_archive_invalidation =
      !prof.empty() && prof[0].auto_archive_invalidation;
  m.instance_settings.deploy_strategy = snapshot.deploy_strategy;

  // Load-order hint: the default profile's resolved plugin list. LOOT is what
  // turns it into a final order, so declare it as a tool the pack needs.
  if (!snapshot.profiles.empty()) {
    m.load_order.plugin_hint = snapshot.profiles.front().load_order;
  }
  if (!m.load_order.plugin_hint.empty()) {
    ManifestTool loot;
    loot.id       = "loot";
    loot.name     = "LOOT";
    loot.homepage = "https://loot.github.io";
    m.tools.push_back(std::move(loot));
  }

  // Platform: the instance's Proton runner is the one per-OS setting an
  // instance actually knows. GMM tries the pin and falls back when that
  // build is not installed locally (the pin is advisory).
  if (!snapshot.proton_runner.empty()) {
    PlatformOverride linux;
    linux.proton_version_pin = snapshot.proton_runner;
    m.platform.linux_plat    = std::move(linux);
  }

  m.choice_groups = options.choice_groups;
  return m;
}

// ---------------------------------------------------------------------------
// build_mod_entries
// ---------------------------------------------------------------------------

std::vector<ModEntry> build_mod_entries(const InstanceSnapshot &snapshot,
                                        const std::filesystem::path &mods_dir,
                                        const PackOptions &options,
                                        const PackCancel *cancel) {
  auto slugs = slug_all(snapshot.mod_entries);

  std::unordered_set<std::string> separators;
  for (const auto &[folder, entry] : snapshot.mod_entries) {
    if (!entry.parent_separator.empty())
      separators.insert(entry.parent_separator);
  }

  struct Row {
    int32_t pos = 0;
    std::string folder;
  };
  std::vector<Row> rows;
  for (const auto &[folder, entry] : snapshot.mod_entries) {
    if (separators.count(folder))
      continue;
    rows.push_back({sort_pos(entry.list_position), folder});
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    return a.pos != b.pos ? a.pos < b.pos : a.folder < b.folder;
  });

  // Does this source have everything updatePolicy "exact" requires? Providers
  // differ: Nexus pins fileId + version + size + hash, the browser-only ones
  // version + hash.
  auto pin_is_complete = [](const ModSource &source) {
    return std::visit(
        [](const auto &s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, ModSourceEmbedded> ||
                        std::is_same_v<T, ModSourceSteamWorkshop>) {
            return false;
          } else if constexpr (std::is_same_v<T, ModSourceNexus>) {
            return s.file_id.has_value() && s.version.has_value() &&
                   s.file_size.has_value() && s.sha256.has_value();
          } else {
            return s.version.has_value() && s.sha256.has_value();
          }
        },
        source);
  };

  std::vector<ModEntry> out;
  report_stage(cancel, PackStage::Sources, static_cast<int>(rows.size()));
  for (const auto &row : rows) {
    if (pack_cancelled(cancel))
      break;
    report_step(cancel, row.folder);
    ModMeta meta = ModMeta::load(mods_dir, row.folder);
    auto source  = resolve_for_export(meta, mods_dir, row.folder, snapshot.game_id,
                                      snapshot.steam_appid, options, cancel);
    if (!source)
      continue;  // nothing to point at: no source, and not bundled
    const auto policy = options.update_policies.find(row.folder);
    if (policy != options.update_policies.end() && policy->second == "exact" &&
        !pin_is_complete(*source)) {
      // User asked to pin this mod. Steam workshop resolution is
      // client-subscription based and an embedded mod carries its bytes in the
      // pack, so exact is meaningless for both - they keep "latest". For the
      // rest, fill the pin from the download's recorded identity and claim
      // "exact" only once every field the schema requires under it is there: an
      // unbacked pin is a pack GMM refuses to reopen.
      const DownloadIdentity id =
          resolve_download_identity(meta, options.downloads_dir);
      std::visit(
          [&](auto &s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, ModSourceNexus>) {
              s.file_size = id.file_size;
              if (!id.sha256.empty())
                s.sha256 = id.sha256;
              if (!id.version.empty())
                s.version = id.version;
            } else if constexpr (!std::is_same_v<T, ModSourceSteamWorkshop> &&
                                 !std::is_same_v<T, ModSourceEmbedded>) {
              if (!id.sha256.empty())
                s.sha256 = id.sha256;
              if (!id.version.empty())
                s.version = id.version;
            }
          },
          *source);
      if (pin_is_complete(*source)) {
        std::visit(
            [](auto &s) {
              s.update_policy = "exact";
            },
            *source);
      }
    }
    ModEntry e;
    e.id       = slugs.at(row.folder);
    e.name     = row.folder;
    e.category = category_for(options.categories.count(row.folder)
                                  ? options.categories.at(row.folder)
                                  : std::string{});
    e.phase    = phase_for(e.category);
    e.source   = std::move(*source);
    out.push_back(std::move(e));
  }
  return out;
}

// ---------------------------------------------------------------------------
// build_executables
// ---------------------------------------------------------------------------

bool is_setup_executable(const std::string &relative_path) {
  const std::string base =
      lower(std::filesystem::path(relative_path).filename().string());
  return setup_executable_name(base);
}

std::vector<ExecutableEntry> build_executables(const InstanceSnapshot &snapshot,
                                               const std::filesystem::path &mods_dir,
                                               const PackCancel *cancel) {
  PackOptions options;
  return build_executables(snapshot, mods_dir, options, cancel);
}

std::vector<ExecutableEntry> build_executables(const InstanceSnapshot &snapshot,
                                               const std::filesystem::path &mods_dir,
                                               const PackOptions &options,
                                               const PackCancel *cancel) {
  // Exported mod ids, to validate sourceModId references.
  std::unordered_set<std::string> exported_ids;
  {
    auto slugs = slug_all(snapshot.mod_entries);
    std::unordered_set<std::string> separators;
    for (const auto &[folder, entry] : snapshot.mod_entries) {
      if (!entry.parent_separator.empty())
        separators.insert(entry.parent_separator);
    }
    report_stage(cancel, PackStage::Executables,
                 static_cast<int>(snapshot.mod_entries.size() - separators.size()));
    for (const auto &[folder, _] : snapshot.mod_entries) {
      if (separators.count(folder))
        continue;
      if (mods_dir.empty()) {
        exported_ids.insert(slugs.at(folder));
        continue;
      }
      if (pack_cancelled(cancel))
        break;
      report_step(cancel, folder);
      ModMeta meta = ModMeta::load(mods_dir, folder);
      if (resolve_for_export(meta, mods_dir, folder, snapshot.game_id,
                             snapshot.steam_appid, options, cancel))
        exported_ids.insert(slugs.at(folder));
    }
  }
  auto slugs = slug_all(snapshot.mod_entries);

  std::vector<ExecutableEntry> out;
  for (const auto &exec : snapshot.executables) {
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
    for (const auto &kv : exec.env) {
      auto eq = kv.find('=');
      if (eq == std::string::npos || eq == 0)
        continue;
      e.env_vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    e.working_dir = exec.cwd;
    // A recognized generator (Nemesis, FNIS, BodySlide, xEdit, ...) runs
    // during install and again whenever its inputs change; anything else is
    // the pack's "Play" launcher and never auto-runs.
    e.role                   = is_setup_executable(exec.path) ? "setup" : "launcher";
    e.auto_run               = e.role == "setup";
    e.rerun_on_modset_change = e.role == "setup";
    out.push_back(std::move(e));
  }
  return out;
}

// ---------------------------------------------------------------------------
// build_ini_entries
// ---------------------------------------------------------------------------

std::vector<IniEntry> build_ini_entries(const std::filesystem::path &mods_dir,
                                        const std::vector<ModEntry> &mods,
                                        const PackCancel *cancel) {
  if (mods_dir.empty())
    return {};

  // INI files are consolidated per target file: every mod's recommended
  // settings for Skyrim.ini land in one ini/Skyrim.ini.json, each as its own
  // named tweak, so a mod can be removed later without touching the others.
  struct Pending {
    std::string tweak_id;
    std::string name;
    std::string content;
    std::string source_mod_id;
  };
  std::map<std::string, std::vector<Pending>> by_target;  // lowercased target
  std::map<std::string, std::string> target_display;      // first-seen casing

  for (const auto &mod : mods) {
    if (mod.id.empty())
      continue;
    if (pack_cancelled(cancel))
      return {};
    report_step(cancel, mod.name);
    // Only the exported mods have a folder on disk under a known name; the
    // mod entry's name is the folder name.
    const std::filesystem::path mod_root = mods_dir / mod.name;
    for (const auto &rel : scan_files(mod_root)) {
      if (is_manager_metadata(rel))
        continue;
      if (lower(std::filesystem::path(rel).extension().string()) != ".ini")
        continue;
      std::string text;
      if (!read_file(mod_root / rel, text))
        continue;
      // Round-trip through the one INI grammar: the tweak content is re-emitted
      // canonical, so what import parses back is what export validated.
      std::string error;
      const auto edits = modpack::parse_ini_content(text, &error);
      if (!edits || edits->empty())
        continue;
      std::string canonical;
      std::string section;
      for (const auto &edit : *edits) {
        if (edit.section != section) {
          section = edit.section;
          canonical += "\n[" + section + "]\n";
        }
        canonical += edit.key + "=" + edit.value + "\n";
      }
      std::string id =
          mod.id + "-" + mod_slug(std::filesystem::path(rel).stem().string());
      by_target[lower(rel)].push_back(Pending{std::move(id),
                                              mod.name + " settings (" + rel + ")",
                                              std::move(canonical), mod.id});
      target_display.emplace(lower(rel), rel);
    }
  }

  std::vector<IniEntry> out;
  for (auto &[target, tweaks] : by_target) {
    IniEntry entry;
    entry.target_file = target_display.at(target);
    for (auto &tweak : tweaks) {
      IniTweak t;
      t.id                = tweak.tweak_id;
      t.name              = tweak.name;
      t.status            = "recommended";  // the user's pick, not pack-critical
      t.enabled           = true;
      t.content           = std::move(tweak.content);
      t.source_mod_id     = tweak.source_mod_id;
      t.has_source_mod_id = true;
      entry.tweaks.push_back(std::move(t));
    }
    out.push_back(std::move(entry));
  }
  return out;
}

// ---------------------------------------------------------------------------
// build_patches
// ---------------------------------------------------------------------------

// A pack should not carry a base64 diff of a game-sized asset.
static constexpr int64_t kMaxPatchFileBytes = 8 * 1024 * 1024;

std::vector<PatchEntry> build_patches(const InstanceSnapshot &snapshot,
                                      const std::filesystem::path &mods_dir,
                                      const std::vector<ModEntry> &mods,
                                      const PackCancel *cancel) {
  if (mods_dir.empty() || mods.size() < 2)
    return {};

  // Mods in list order: the earlier one wins the conflict, every later one
  // becomes an opt-in patch against it. mods[] is already list-ordered.
  struct Owner {
    std::string mod_id;
    std::string name;
    int32_t pos = 0;
  };
  std::unordered_map<std::string, std::vector<Owner>> by_path;  // relative path
  report_stage(cancel, PackStage::Patches, static_cast<int>(mods.size()));
  for (const auto &mod : mods) {
    report_step(cancel, mod.name);
    const auto it     = snapshot.mod_entries.find(mod.name);
    const int32_t pos = it == snapshot.mod_entries.end()
                            ? INT32_MAX
                            : sort_pos(it->second.list_position);
    for (const auto &rel : scan_files(mods_dir / mod.name)) {
      if (is_manager_metadata(rel))
        continue;
      by_path[rel].push_back(Owner{mod.id, mod.name, pos});
    }
  }

  // Per mod, its patches in discovery order become a contiguous chain
  // (patches/<mod-id>-<N>.json); a single patch keeps the plain filename.
  std::unordered_map<std::string, std::vector<PatchEntry>> by_mod;
  std::vector<std::string> mod_order;
  // The walk above is done, so the colliding paths are known: from here the unit
  // is one file to binary-diff, which is the stage the time goes into.
  int colliding = 0;
  for (const auto &entry : by_path) {
    if (entry.second.size() >= 2)
      ++colliding;
  }
  report_stage(cancel, PackStage::Patches, colliding);
  for (const auto &[rel, owners] : by_path) {
    if (owners.size() < 2)
      continue;
    // One bsdiff per colliding file below: stop before starting the next.
    if (pack_cancelled(cancel))
      break;
    report_step(cancel, rel);
    auto sorted = owners;
    std::sort(sorted.begin(), sorted.end(), [](const Owner &a, const Owner &b) {
      return a.pos != b.pos ? a.pos < b.pos : a.mod_id < b.mod_id;
    });
    std::string base;
    std::string base_hash;
    for (size_t i = 0; i < sorted.size(); ++i) {
      if (pack_cancelled(cancel))
        break;
      const std::filesystem::path file = mods_dir / sorted[i].name / rel;
      std::error_code ec;
      const auto size = std::filesystem::file_size(file, ec);
      if (ec || size == 0 || size > static_cast<uintmax_t>(kMaxPatchFileBytes))
        continue;
      std::string content;
      if (!read_file(file, content))
        continue;
      if (i == 0) {
        base      = content;
        base_hash = sha256_hex(content);
        continue;
      }
      if (base == content)
        continue;  // identical bytes: nothing to patch
      std::vector<uint8_t> payload;
      std::string error;
      if (!bsdiff_create(reinterpret_cast<const uint8_t *>(base.data()), base.size(),
                         reinterpret_cast<const uint8_t *>(content.data()),
                         content.size(), payload, error)) {
        continue;
      }
      PatchEntry p;
      p.mod_id           = sorted[i].mod_id;
      p.target_path      = rel;
      p.base_file_sha256 = base_hash;
      p.algorithm        = "bsdiff";
      p.payload_base64   = base64_encode(payload);
      if (!by_mod.contains(p.mod_id))
        mod_order.push_back(p.mod_id);
      by_mod[p.mod_id].push_back(std::move(p));
    }
  }
  std::sort(mod_order.begin(), mod_order.end());

  std::vector<PatchEntry> out;
  for (const auto &mod_id : mod_order) {
    auto &entries = by_mod[mod_id];
    if (entries.size() == 1) {
      out.push_back(std::move(entries.front()));
      continue;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
      entries[i].sequence = static_cast<int>(i + 1);
      out.push_back(std::move(entries[i]));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// JSON serializers (reverse of unpacker.cpp parse_*)
// ---------------------------------------------------------------------------

nlohmann::json serialize_mod_source(const ModSource &source) {
  return std::visit(
      [](const auto &s) -> nlohmann::json {
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
          if (s.md5)
            j["md5"] = *s.md5;
          j["updatePolicy"] = s.update_policy;
        } else if constexpr (std::is_same_v<T, ModSourceLoversLab>) {
          std::visit(
              [&](const auto &id) {
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
              [&](const auto &id) {
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
        } else if constexpr (std::is_same_v<T, ModSourceEmbedded>) {
          j["root"]      = s.root;
          j["fileCount"] = s.files.size();
          j["files"]     = nlohmann::json::array();
          for (const auto &f : s.files) {
            j["files"].push_back(
                {{"path", f.path}, {"size", f.size}, {"sha256", f.sha256}});
          }
          j["updatePolicy"] = s.update_policy;
        }
        return j;
      },
      source);
}

static std::string category_name(ModCategory c) {
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

nlohmann::json serialize_mod_entry(const ModEntry &m) {
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
    for (const auto &[k, v] : m.installer_choices->selections)
      sel[k] = v;
    ic["selections"]      = std::move(sel);
    j["installerChoices"] = std::move(ic);
  }
  return j;
}

// ini/<targetFile>.json - reverse of parse_ini_entry (ini_edit_parser.cpp).
nlohmann::json serialize_ini_entry(const IniEntry &entry) {
  nlohmann::json j;
  j["targetFile"] = entry.target_file;
  j["tweaks"]     = nlohmann::json::array();
  for (const auto &tweak : entry.tweaks) {
    j["tweaks"].push_back(
        {{"id", tweak.id},
         {"name", tweak.name},
         {"status", tweak.status},
         {"enabled", tweak.enabled},
         {"sourceModId", tweak.has_source_mod_id ? nlohmann::json(tweak.source_mod_id)
                                                 : nlohmann::json(nullptr)},
         {"content", tweak.content}});
  }
  return j;
}

// patches/<id>[-N].json - reverse of parse_patch_entry. The filename carries
// the same mod id and sequence, which import cross-checks.
nlohmann::json serialize_patch_entry(const PatchEntry &p) {
  nlohmann::json j;
  j["modId"] = p.mod_id;
  if (p.sequence)
    j["sequence"] = *p.sequence;
  j["targetPath"]     = p.target_path;
  j["baseFileSha256"] = p.base_file_sha256;
  j["algorithm"]      = p.algorithm;
  j["payloadBase64"]  = p.payload_base64;
  return j;
}

std::string patch_archive_path(const PatchEntry &p) {
  std::string name = "patches/" + p.mod_id;
  if (p.sequence)
    name += "-" + std::to_string(*p.sequence);
  return name + ".json";
}

nlohmann::json serialize_executable_entry(const ExecutableEntry &e) {
  nlohmann::json j;
  j["id"]           = e.id;
  j["sourceModId"]  = e.source_mod_id;
  j["relativePath"] = e.relative_path;
  j["role"]         = e.role;
  if (!e.arguments.empty())
    j["arguments"] = e.arguments;
  if (!e.env_vars.empty()) {
    nlohmann::json env = nlohmann::json::object();
    for (const auto &[k, v] : e.env_vars)
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

nlohmann::json serialize_manifest(const Manifest &m) {
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
    for (const auto &t : m.tools) {
      nlohmann::json tj;
      tj["id"]   = t.id;
      tj["name"] = t.name;
      if (!t.homepage.empty())
        tj["homepage"] = t.homepage;
      j["tools"].push_back(std::move(tj));
    }
  }
  auto ser_plat = [](const std::optional<PlatformOverride> &plat) {
    nlohmann::json pj;
    if (!plat->proton_version_pin.empty())
      pj["protonVersionPin"] = plat->proton_version_pin;
    if (plat->steam_overlay)
      pj["steamOverlay"] = *plat->steam_overlay;
    if (!plat->launch_options.empty())
      pj["launchOptions"] = plat->launch_options;
    if (!plat->prefix_files.empty()) {
      pj["prefixFiles"] = nlohmann::json::array();
      for (const auto &pf : plat->prefix_files) {
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
    for (const auto &r : m.rules) {
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
    for (const auto &cg : m.choice_groups) {
      j["choiceGroups"].push_back({{"id", cg.id},
                                   {"name", cg.name},
                                   {"mode", cg.mode},
                                   {"memberModIds", cg.member_mod_ids}});
    }
  }
  nlohmann::json fh = nlohmann::json::object();
  for (const auto &[path, hash] : m.archive.file_hashes)
    fh[path] = hash;
  j["archive"] = {{"fileHashes", std::move(fh)}};
  nlohmann::json is;
  is["localSaves"]                   = m.instance_settings.local_saves;
  is["localSettings"]                = m.instance_settings.local_settings;
  is["automaticArchiveInvalidation"] = m.instance_settings.auto_archive_invalidation;
  if (!m.instance_settings.deploy_strategy.empty())
    is["deployStrategy"] = m.instance_settings.deploy_strategy;
  j["instanceSettings"] = is;
  return j;
}

// ---------------------------------------------------------------------------
// build_gmmpack
// ---------------------------------------------------------------------------

Gmmpack build_gmmpack(const InstanceSnapshot &snapshot,
                      const std::filesystem::path &mods_dir, const PackOptions &options,
                      const PackCancel *cancel) {
  Gmmpack pack;
  pack.manifest    = build_manifest(snapshot, options);
  pack.mods        = build_mod_entries(snapshot, mods_dir, options, cancel);
  pack.executables = build_executables(snapshot, mods_dir, options, cancel);
  pack.ini_edits   = build_ini_entries(mods_dir, pack.mods, cancel);
  pack.patches     = build_patches(snapshot, mods_dir, pack.mods, cancel);
  pack.tree        = build_tree(snapshot, mods_dir, options, cancel);
  // Rules need both sides built: an executable needs its source mod on disk
  // before it can run, and executable ids share the mod id-space for
  // before/after/requires (see the format spec's Executables section).
  for (const auto &exe : pack.executables) {
    ManifestRule rule;
    rule.type = "requires";
    rule.from = exe.id;
    rule.to   = exe.source_mod_id;
    pack.manifest.rules.push_back(std::move(rule));
  }
  if (!options.instructions.empty())
    pack.instructions = options.instructions;

  // Payload for bundled mods: read once here so create_gmmpack can write the
  // exact bytes the mod entries' hashes were computed over.
  {
    int bundled_files = 0;
    for (const auto &mod : pack.mods) {
      const auto *src = std::get_if<ModSourceEmbedded>(&mod.source);
      if (src != nullptr)
        bundled_files += static_cast<int>(src->files.size());
    }
    report_stage(cancel, PackStage::Payload, bundled_files);
    if (!mods_dir.empty()) {
      for (const auto &mod : pack.mods) {
        const auto *src = std::get_if<ModSourceEmbedded>(&mod.source);
        if (src == nullptr)
          continue;
        const std::filesystem::path root = mods_dir / mod.name;
        for (const auto &f : src->files) {
          if (pack_cancelled(cancel))
            return pack;
          // The stage's unit is the file here, so the mod is only the name the
          // file count is read against.
          report_step(cancel, root_prefix(*src) + f.path);
          std::string content;
          if (!read_file(root / f.path, content))
            continue;
          pack.payload.push_back({root_prefix(*src) + f.path, std::move(content)});
        }
      }
    }
  }
  return pack;
}

// ---------------------------------------------------------------------------
// Archive writing
// ---------------------------------------------------------------------------

static bool write_zip_entry(struct archive *a, const std::string &path,
                            const std::string &content, std::string &error) {
  struct archive_entry *e = archive_entry_new();
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

PackResult create_gmmpack(const InstanceSnapshot &snapshot,
                          const std::filesystem::path &mods_dir,
                          const PackOptions &options,
                          const std::filesystem::path &output_path) {
  PackResult result;
  result.output_path = output_path;

  Gmmpack pack = build_gmmpack(snapshot, mods_dir, options);

  // Serialize every payload first; hash them into the manifest.
  std::vector<std::pair<std::string, std::string>> files;
  for (const auto &mod : pack.mods) {
    if (std::holds_alternative<ModSourceEmbedded>(mod.source))
      ++result.embedded_mod_count;
    files.emplace_back("mods/" + mod.id + ".json", serialize_mod_entry(mod).dump(2));
  }
  for (const auto &exec : pack.executables) {
    files.emplace_back("executables/" + exec.id + ".json",
                       serialize_executable_entry(exec).dump(2));
  }
  for (const auto &entry : pack.ini_edits) {
    files.emplace_back("ini/" + entry.target_file + ".json",
                       serialize_ini_entry(entry).dump(2));
  }
  for (const auto &patch : pack.patches) {
    files.emplace_back(patch_archive_path(patch), serialize_patch_entry(patch).dump(2));
  }
  for (const auto &af : pack.payload) {
    result.embedded_file_count++;
    files.emplace_back(af.path, af.content);
  }
  files.emplace_back("tree.json", serialize_tree(pack.tree).dump(2));
  if (pack.instructions)
    files.emplace_back("instructions.md", *pack.instructions);

  for (const auto &[path, content] : files) {
    pack.manifest.archive.file_hashes[path] = "sha256:" + sha256_hex(content);
  }
  const std::string manifest_json = serialize_manifest(pack.manifest).dump(2);

  // Validate what we just built, with the same two stages the importer runs.
  // A pack that fails here is a pack GMM cannot reopen: refuse to write it
  // instead of reporting a success the user's own import will reject.
  {
    ArchiveContents archive;
    archive.path_index["manifest.json"] = 0;
    archive.files.push_back({"manifest.json", manifest_json});
    for (const auto &[path, content] : files) {
      archive.path_index[path] = archive.files.size();
      archive.files.push_back({path, content});
    }
    const nlohmann::json manifest_parsed = nlohmann::json::parse(manifest_json);

    Diagnostics diagnostics;
    if (!options.schema_dir.empty()) {
      diagnostics = validate_schemas(archive, manifest_parsed,
                                     load_schema_set(options.schema_dir));
    }
    const Diagnostics refs = check_referential_integrity(pack);
    diagnostics.insert(diagnostics.end(), refs.begin(), refs.end());

    std::string first_error;
    for (const auto &d : diagnostics) {
      if (d.severity != Diagnostic::Severity::Error)
        continue;
      first_error = d.path.empty() ? d.message : d.path + ": " + d.message;
      break;
    }
    if (!first_error.empty()) {
      result.error = "the pack did not validate - " + first_error;
      return result;
    }
  }

  struct archive *a = archive_write_new();
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
  for (const auto &[path, content] : files) {
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
