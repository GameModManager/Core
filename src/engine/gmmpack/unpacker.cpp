#include "engine/gmmpack/unpacker.h"
#include "engine/gmmpack/ini_edit_parser.h"
#include "engine/gmmpack/schema_validator.h"
#include "engine/gmmpack/tree_parser.h"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace engine::gmmpack {

// ---------------------------------------------------------------------------
// SHA-256 via OpenSSL EVP
// ---------------------------------------------------------------------------

static std::string sha256_hex(const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        result.push_back(hex[hash[i] >> 4]);
        result.push_back(hex[hash[i] & 0x0f]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// libarchive helpers
// ---------------------------------------------------------------------------

static std::string archive_error_msg(struct archive* a) {
    const char* msg = archive_error_string(a);
    return msg ? msg : "unknown libarchive error";
}

// ---------------------------------------------------------------------------
// Schema set loading
// ---------------------------------------------------------------------------

SchemaSet load_schema_set(const std::filesystem::path& schema_dir) {
    SchemaSet schemas;
    static const char* names[] = {
        "manifest.schema.json", "mod.schema.json", "executable.schema.json",
        "patch.schema.json",    "ini.schema.json", "tree.schema.json",
    };
    for (const char* name : names) {
        auto p = schema_dir / name;
        if (std::filesystem::exists(p)) {
            std::ifstream f(p);
            if (f.is_open()) {
                schemas[name] = nlohmann::json::parse(f);
            }
        }
    }
    return schemas;
}

// ---------------------------------------------------------------------------
// Stage 1: Extract archive into memory
// ---------------------------------------------------------------------------

ExtractResult extract_archive(const std::filesystem::path& archive_path) {
    ExtractResult result;

    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, archive_path.string().c_str(),
                                   10240) != ARCHIVE_OK) {
        result.diagnostics.push_back(
            {Diagnostic::Severity::Error, "",
             "cannot open archive: " + archive_error_msg(a)});
        archive_read_free(a);
        return result;
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) == AE_IFDIR) continue;

        std::string path = archive_entry_pathname(entry);
        la_int64_t size = archive_entry_size(entry);

        std::string content;
        content.resize(static_cast<size_t>(size));

        ssize_t total_read = 0;
        const void* buf;
        size_t buf_size;
        la_int64_t offset;

        while (total_read < size) {
            int r = archive_read_data_block(a, &buf, &buf_size, &offset);
            if (r == ARCHIVE_EOF || r == ARCHIVE_OK) {
                if (buf_size == 0) break;
                size_t to_copy =
                    std::min(static_cast<size_t>(size - total_read), buf_size);
                std::memcpy(content.data() + total_read, buf, to_copy);
                total_read += to_copy;
            } else {
                result.diagnostics.push_back(
                    {Diagnostic::Severity::Error, "",
                     "read error in " + path + ": " + archive_error_msg(a)});
                archive_read_close(a);
                archive_read_free(a);
                return result;
            }
        }

        content.resize(static_cast<size_t>(total_read));

        size_t idx = result.archive.files.size();
        result.archive.path_index[path] = idx;
        result.archive.files.push_back({std::move(path), std::move(content)});
    }

    archive_read_close(a);
    archive_read_free(a);

    // Parse manifest.json
    auto it = result.archive.path_index.find("manifest.json");
    if (it == result.archive.path_index.end()) {
        result.diagnostics.push_back(
            {Diagnostic::Severity::Error, "",
             "archive does not contain manifest.json"});
        return result;
    }

    try {
        result.manifest_json = nlohmann::json::parse(
            result.archive.files[it->second].content);
    } catch (const nlohmann::json::parse_error& e) {
        result.diagnostics.push_back(
            {Diagnostic::Severity::Error, "manifest.json",
             std::string("JSON parse error: ") + e.what()});
        return result;
    }

    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Stage 1b: Archive integrity (sha256 per file vs manifest.fileHashes)
// ---------------------------------------------------------------------------

Diagnostics verify_archive_integrity(const ArchiveContents& archive,
                                     const nlohmann::json& manifest_json) {
    Diagnostics diag;

    if (!manifest_json.contains("archive") ||
        !manifest_json["archive"].is_object() ||
        !manifest_json["archive"].contains("fileHashes") ||
        !manifest_json["archive"]["fileHashes"].is_object()) {
        diag.push_back({Diagnostic::Severity::Error, "archive.fileHashes",
                        "missing fileHashes in manifest"});
        return diag;
    }

    const auto& hashes = manifest_json["archive"]["fileHashes"];

    // Check every file in fileHashes exists and matches
    for (auto it = hashes.begin(); it != hashes.end(); ++it) {
        std::string path = it.key();
        if (!it.value().is_string()) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "archive.fileHashes." + path,
                 "hash must be a string"});
            continue;
        }
        std::string expected = it.value().get<std::string>();

        // Strip "sha256:" prefix
        if (expected.rfind("sha256:", 0) != 0) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "archive.fileHashes." + path,
                 "hash must start with sha256:"});
            continue;
        }
        std::string expected_hex = expected.substr(7);

        auto fidx = archive.path_index.find(path);
        if (fidx == archive.path_index.end()) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "archive.fileHashes." + path,
                 "file listed in fileHashes but not in archive"});
            continue;
        }

        std::string actual_hex =
            sha256_hex(archive.files[fidx->second].content);
        if (actual_hex != expected_hex) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "archive.fileHashes." + path,
                 "sha256 mismatch: expected " + expected_hex +
                     ", got " + actual_hex});
        }
    }

    // Check for files in archive not listed in fileHashes (except manifest.json)
    for (const auto& f : archive.files) {
        if (f.path == "manifest.json") continue;
        if (!hashes.contains(f.path)) {
            diag.push_back(
                {Diagnostic::Severity::Error, f.path,
                 "file in archive but not listed in fileHashes"});
        }
    }

    return diag;
}

// ---------------------------------------------------------------------------
// Stage 2: Schema validation
// ---------------------------------------------------------------------------

Diagnostics validate_schemas(const ArchiveContents& archive,
                             const nlohmann::json& manifest_json,
                             const SchemaSet& schemas) {
    Diagnostics diag;
    SchemaValidator validator;

    // Validate manifest.json against manifest.schema.json
    if (schemas.count("manifest.schema.json")) {
        const auto& schema = schemas.at("manifest.schema.json");
        auto manifest_diag =
            validator.validate(manifest_json, schema, schema);
        diag.insert(diag.end(), manifest_diag.begin(), manifest_diag.end());
    }

    // Validate each file against its schema
    for (const auto& f : archive.files) {
        if (f.path == "manifest.json" || f.path == "instructions.md") continue;

        std::string schema_name;
        if (f.path.rfind("mods/", 0) == 0)
            schema_name = "mod.schema.json";
        else if (f.path.rfind("executables/", 0) == 0)
            schema_name = "executable.schema.json";
        else if (f.path.rfind("patches/", 0) == 0)
            schema_name = "patch.schema.json";
        else if (f.path.rfind("ini/", 0) == 0)
            schema_name = "ini.schema.json";
        else if (f.path == "tree.json")
            schema_name = "tree.schema.json";
        else
            continue;  // unknown path, skip (not in schema set)

        if (schemas.count(schema_name) == 0) continue;

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(f.content);
        } catch (const nlohmann::json::parse_error& e) {
            diag.push_back(
                {Diagnostic::Severity::Error, f.path,
                 std::string("JSON parse error: ") + e.what()});
            continue;
        }

        const auto& schema = schemas.at(schema_name);
        auto file_diag = validator.validate(parsed, schema, schema);
        for (auto& d : file_diag) {
            if (!d.path.empty())
                d.path = f.path + "/" + d.path;
            else
                d.path = f.path;
        }
        diag.insert(diag.end(), file_diag.begin(), file_diag.end());
    }

    return diag;
}

// ---------------------------------------------------------------------------
// Stage 3: Referential integrity
// ---------------------------------------------------------------------------

Diagnostics check_referential_integrity(const Gmmpack& pack) {
    Diagnostics diag;

    // Build ID sets
    std::unordered_set<std::string> mod_ids;
    for (const auto& m : pack.mods) mod_ids.insert(m.id);

    std::unordered_set<std::string> exec_ids;
    for (const auto& e : pack.executables) exec_ids.insert(e.id);

    // syntheticModId from executables
    std::unordered_set<std::string> synthetic_ids;
    for (const auto& e : pack.executables) {
        if (e.output && e.output->capture == "syntheticMod") {
            synthetic_ids.insert(e.output->synthetic_mod_id);
        }
    }

    auto is_valid_id = [&](const std::string& id) {
        return mod_ids.count(id) || exec_ids.count(id) ||
               synthetic_ids.count(id);
    };
    auto is_valid_mod_id = [&](const std::string& id) {
        return mod_ids.count(id) > 0;
    };

    // rules[].from / rules[].to must resolve to a real mod or executable id
    for (size_t i = 0; i < pack.manifest.rules.size(); ++i) {
        const auto& rule = pack.manifest.rules[i];
        std::string prefix = "rules[" + std::to_string(i) + "]";
        if (!is_valid_id(rule.from)) {
            diag.push_back({Diagnostic::Severity::Error, prefix + ".from",
                            "unknown id: " + rule.from});
        }
        if (!is_valid_id(rule.to)) {
            diag.push_back({Diagnostic::Severity::Error, prefix + ".to",
                            "unknown id: " + rule.to});
        }
    }

    // choiceGroups[].memberModIds[] must resolve to real mod ids
    for (size_t i = 0; i < pack.manifest.choice_groups.size(); ++i) {
        const auto& cg = pack.manifest.choice_groups[i];
        std::string prefix =
            "choiceGroups[" + std::to_string(i) + "]";
        for (size_t j = 0; j < cg.member_mod_ids.size(); ++j) {
            if (!is_valid_mod_id(cg.member_mod_ids[j])) {
                diag.push_back(
                    {Diagnostic::Severity::Error,
                     prefix + ".memberModIds[" + std::to_string(j) + "]",
                     "unknown mod id: " + cg.member_mod_ids[j]});
            }
        }
    }

    // executables[].sourceModId must resolve to a real mod id
    for (size_t i = 0; i < pack.executables.size(); ++i) {
        const auto& e = pack.executables[i];
        if (!is_valid_mod_id(e.source_mod_id)) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "executables/" + e.id + ".json.sourceModId",
                 "unknown mod id: " + e.source_mod_id});
        }
    }

    // tree.json mod nodes' id must resolve to a real mod id or syntheticModId
    std::function<void(const std::vector<TreeNode>&, const std::string&)> check_tree =
        [&](const std::vector<TreeNode>& nodes, const std::string& prefix) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (auto* sep = std::get_if<SeparatorNode>(&nodes[i].data)) {
                    check_tree(sep->children,
                               prefix + "nodes[" + std::to_string(i) +
                                   "].children");
                } else if (auto* mod =
                               std::get_if<ModNode>(&nodes[i].data)) {
                    if (!is_valid_mod_id(mod->id) &&
                        !synthetic_ids.count(mod->id)) {
                        diag.push_back(
                            {Diagnostic::Severity::Error,
                             prefix + "nodes[" + std::to_string(i) + "].id",
                             "unknown mod id: " + mod->id});
                    }
                }
            }
        };
    check_tree(pack.tree.nodes, "");

    // patches/*.json: modId must match filename id, and filename cross-check
    for (const auto& p : pack.patches) {
        if (!is_valid_mod_id(p.mod_id)) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 p.archive_path.empty()
                     ? "patches/<unknown>.json.modId"
                     : p.archive_path + ".modId",
                 "unknown mod id: " + p.mod_id});
        }

        // Cross-check filename mod_id/sequence vs JSON modId/sequence fields
        if (!p.archive_path.empty()) {
            auto parsed_name = parse_patch_filename(p.archive_path);
            if (parsed_name) {
                const auto& filename_mod_id = parsed_name->first;
                if (filename_mod_id != p.mod_id) {
                    diag.push_back(
                        {Diagnostic::Severity::Error,
                         p.archive_path,
                         "filename mod_id '" + filename_mod_id +
                             "' does not match modId field '" + p.mod_id +
                             "'"});
                }
                int filename_seq = parsed_name->second;
                if (filename_seq == 0 && p.sequence.has_value()) {
                    diag.push_back(
                        {Diagnostic::Severity::Error,
                         p.archive_path,
                         "single patch file must not carry a sequence field"});
                } else if (filename_seq != 0 && !p.sequence.has_value()) {
                    diag.push_back(
                        {Diagnostic::Severity::Error,
                         p.archive_path,
                         "chain patch file '" + p.archive_path +
                             "' is missing its sequence field"});
                } else if (filename_seq != 0 && *p.sequence != filename_seq) {
                    diag.push_back(
                        {Diagnostic::Severity::Error,
                         p.archive_path,
                         "filename sequence " +
                             std::to_string(filename_seq) +
                             " does not match sequence field " +
                             std::to_string(*p.sequence)});
                }
            }
        }
    }

// patches: a mod has either one single patch or a chain, never both;
    // chain sequences must be contiguous starting at 1
    std::unordered_map<std::string, std::vector<int>> patch_sequences;
    std::unordered_map<std::string, int> patch_single_counts;
    for (const auto& p : pack.patches) {
        if (p.sequence) {
            patch_sequences[p.mod_id].push_back(*p.sequence);
        } else {
            patch_single_counts[p.mod_id]++;
        }
    }
    for (const auto& [mod_id, single_count] : patch_single_counts) {
        if (patch_sequences.contains(mod_id)) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "patches/" + mod_id,
                 "mod has both a single patch and chained patches"});
        }
    }
    for (const auto& [mod_id, seqs] : patch_sequences) {
        auto sorted = seqs;
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (sorted[i] != static_cast<int>(i + 1)) {
                diag.push_back(
                    {Diagnostic::Severity::Error,
                     "patches/" + mod_id,
                     "non-contiguous sequence: expected " +
                         std::to_string(i + 1) + ", got " +
                         std::to_string(sorted[i])});
            }
        }
    }

// ini/*.json: tweak sourceModId (when non-null) must resolve to a real
    // mod id; tweak ids must be unique within the file (uniqueness is not
    // expressible in JSON Schema, so it is checked here).
    for (size_t fi = 0; fi < pack.ini_edits.size(); ++fi) {
        const auto& ini = pack.ini_edits[fi];
        std::unordered_set<std::string> seen_ids;
        for (size_t ti = 0; ti < ini.tweaks.size(); ++ti) {
            const auto& tweak = ini.tweaks[ti];
            const std::string where =
                "ini/" + ini.target_file + ".tweaks[" + std::to_string(ti) + "]";
            if (!seen_ids.insert(tweak.id).second) {
                diag.push_back({Diagnostic::Severity::Error, where + ".id",
                                "duplicate tweak id: " + tweak.id});
            }
            if (tweak.has_source_mod_id &&
                !is_valid_mod_id(tweak.source_mod_id)) {
                diag.push_back({Diagnostic::Severity::Error,
                                where + ".sourceModId",
                                "unknown mod id: " + tweak.source_mod_id});
            }
        }
    }

    // platform.prefixFiles[].sourceModId must resolve to a real mod id
    auto check_platform_prefix_files =
        [&](const std::optional<PlatformOverride>& plat,
            const std::string& os_name) {
            if (!plat) return;
            for (size_t i = 0; i < plat->prefix_files.size(); ++i) {
                const auto& pf = plat->prefix_files[i];
                if (!pf.source_mod_id.empty() &&
                    !is_valid_mod_id(pf.source_mod_id)) {
                    diag.push_back(
                        {Diagnostic::Severity::Error,
                         "platform." + os_name + ".prefixFiles[" +
                             std::to_string(i) + "].sourceModId",
                         "unknown mod id: " + pf.source_mod_id});
                }
            }
        };
    check_platform_prefix_files(pack.manifest.platform.linux_plat, "linux");
    check_platform_prefix_files(pack.manifest.platform.macos, "macos");
    check_platform_prefix_files(pack.manifest.platform.windows, "windows");

    return diag;
}

// ---------------------------------------------------------------------------
// JSON -> typed struct parsing
// ---------------------------------------------------------------------------

Manifest parse_manifest(const nlohmann::json& j) {
    Manifest m;
    m.gmmpack_schema = j.value("gmmpackSchema", "");
    m.id = j.value("id", "");
    m.revision = j.value("revision", 0);

    if (j.contains("info")) {
        const auto& info = j["info"];
        m.info.name = info.value("name", "");
        m.info.author = info.value("author", "");
        m.info.description = info.value("description", "");
        m.info.gmm_game_id = info.value("gmmGameId", "");
        m.info.homepage = info.value("homepage", "");
        m.info.created_at = info.value("createdAt", "");
        m.info.updated_at = info.value("updatedAt", "");
    }

    if (j.contains("tools")) {
        for (const auto& t : j["tools"]) {
            ManifestTool tool;
            tool.id = t.value("id", "");
            tool.name = t.value("name", "");
            tool.homepage = t.value("homepage", "");
            m.tools.push_back(std::move(tool));
        }
    }

    auto parse_platform_override = [](const nlohmann::json& j) {
        PlatformOverride po;
        po.proton_version_pin = j.value("protonVersionPin", "");
        if (j.contains("steamOverlay"))
            po.steam_overlay = j["steamOverlay"].get<bool>();
        po.launch_options = j.value("launchOptions", "");
        if (j.contains("prefixFiles")) {
            for (const auto& pf : j["prefixFiles"]) {
                PlatformPrefixFile ppf;
                ppf.path = pf.value("path", "");
                ppf.source_mod_id = pf.value("sourceModId", "");
                po.prefix_files.push_back(std::move(ppf));
            }
        }
        return po;
    };

    if (j.contains("platform")) {
        const auto& plat = j["platform"];
        if (plat.contains("linux"))
            m.platform.linux_plat = parse_platform_override(plat["linux"]);
        if (plat.contains("macos"))
            m.platform.macos = parse_platform_override(plat["macos"]);
        if (plat.contains("windows"))
            m.platform.windows = parse_platform_override(plat["windows"]);
    }

    if (j.contains("rules")) {
        for (const auto& r : j["rules"]) {
            ManifestRule rule;
            rule.type = r.value("type", "");
            rule.from = r.value("from", "");
            rule.to = r.value("to", "");
            rule.note = r.value("note", "");
            m.rules.push_back(std::move(rule));
        }
    }

    if (j.contains("loadOrder")) {
        const auto& lo = j["loadOrder"];
        if (lo.contains("pluginHint")) {
            for (const auto& h : lo["pluginHint"]) {
                m.load_order.plugin_hint.push_back(h.get<std::string>());
            }
        }
    }

    if (j.contains("choiceGroups")) {
        for (const auto& cg : j["choiceGroups"]) {
            ChoiceGroup group;
            group.id = cg.value("id", "");
            group.name = cg.value("name", "");
            group.mode = cg.value("mode", "");
            if (cg.contains("memberModIds")) {
                for (const auto& id : cg["memberModIds"]) {
                    group.member_mod_ids.push_back(id.get<std::string>());
                }
            }
            m.choice_groups.push_back(std::move(group));
        }
    }

    if (j.contains("archive") && j["archive"].contains("fileHashes")) {
        for (auto it = j["archive"]["fileHashes"].begin();
             it != j["archive"]["fileHashes"].end(); ++it) {
            m.archive.file_hashes[it.key()] = it.value().get<std::string>();
        }
    }

    return m;
}

static ModSource parse_mod_source(const nlohmann::json& j) {
    std::string provider = j.value("provider", "");

    if (provider == "nexus") {
        ModSourceNexus s;
        s.resolution = j.value("resolution", "");
        s.game_domain = j.value("gameDomain", "");
        s.mod_id = j.value("modId", int64_t(0));
        if (j.contains("fileId")) s.file_id = j["fileId"].get<int64_t>();
        if (j.contains("version")) s.version = j["version"].get<std::string>();
        if (j.contains("fileName"))
            s.file_name = j["fileName"].get<std::string>();
        if (j.contains("fileSize"))
            s.file_size = j["fileSize"].get<int64_t>();
        if (j.contains("sha256")) s.sha256 = j["sha256"].get<std::string>();
        s.update_policy = j.value("updatePolicy", "exact");
        return s;
    }
    if (provider == "loverslab") {
        ModSourceLoversLab s;
        s.resolution = j.value("resolution", "browser");
        if (j["modId"].is_number())
            s.mod_id = j["modId"].get<int64_t>();
        else
            s.mod_id = j["modId"].get<std::string>();
        s.section_slug = j.value("sectionSlug", "");
        if (j.contains("version")) s.version = j["version"].get<std::string>();
        if (j.contains("fileName"))
            s.file_name = j["fileName"].get<std::string>();
        if (j.contains("sha256")) s.sha256 = j["sha256"].get<std::string>();
        s.update_policy = j.value("updatePolicy", "exact");
        return s;
    }
    if (provider == "modpub") {
        ModSourceModPub s;
        s.resolution = j.value("resolution", "browser");
        if (j["modId"].is_number())
            s.mod_id = j["modId"].get<int64_t>();
        else
            s.mod_id = j["modId"].get<std::string>();
        if (j.contains("version")) s.version = j["version"].get<std::string>();
        if (j.contains("fileName"))
            s.file_name = j["fileName"].get<std::string>();
        if (j.contains("sha256")) s.sha256 = j["sha256"].get<std::string>();
        s.update_policy = j.value("updatePolicy", "exact");
        return s;
    }
    if (provider == "steam_workshop") {
        ModSourceSteamWorkshop s;
        s.resolution = j.value("resolution", "client-subscription");
        s.app_id = j.value("appId", int64_t(0));
        s.workshop_item_id = j.value("workshopItemId", int64_t(0));
        if (j.contains("version") && !j["version"].is_null())
            s.version = j["version"].get<std::string>();
        s.update_policy = j.value("updatePolicy", "latest");
        return s;
    }
    if (provider == "direct") {
        ModSourceDirect s;
        s.resolution = j.value("resolution", "");
        s.url = j.value("url", "");
        if (j.contains("version")) s.version = j["version"].get<std::string>();
        if (j.contains("fileName"))
            s.file_name = j["fileName"].get<std::string>();
        if (j.contains("sha256")) s.sha256 = j["sha256"].get<std::string>();
        s.update_policy = j.value("updatePolicy", "exact");
        return s;
    }

    // Unknown provider - return default nexus with empty fields
    return ModSourceNexus{};
}

static ModCategory parse_category(const std::string& s) {
    if (s == "required") return ModCategory::Required;
    if (s == "recommended") return ModCategory::Recommended;
    return ModCategory::Optional;
}

ModEntry parse_mod_entry(const nlohmann::json& j) {
    ModEntry m;
    m.id = j.value("id", "");
    m.name = j.value("name", "");
    m.phase = j.value("phase", 0);
    m.category = parse_category(j.value("category", ""));
    if (j.contains("source")) m.source = parse_mod_source(j["source"]);
    if (j.contains("installerChoices")) {
        InstallerChoices ic;
        const auto& icj = j["installerChoices"];
        ic.type = icj.value("type", "");
        if (icj.contains("selections")) {
            for (auto it = icj["selections"].begin();
                 it != icj["selections"].end(); ++it) {
                std::vector<std::string> vals;
                for (const auto& v : *it)
                    vals.push_back(v.get<std::string>());
                ic.selections[it.key()] = std::move(vals);
            }
        }
        m.installer_choices = std::move(ic);
    }
    return m;
}

ExecutableEntry parse_executable_entry(const nlohmann::json& j) {
    ExecutableEntry e;
    e.id = j.value("id", "");
    e.source_mod_id = j.value("sourceModId", "");
    e.relative_path = j.value("relativePath", "");
    if (j.contains("arguments")) {
        for (const auto& a : j["arguments"])
            e.arguments.push_back(a.get<std::string>());
    }
    if (j.contains("envVars")) {
        for (auto it = j["envVars"].begin(); it != j["envVars"].end(); ++it)
            e.env_vars[it.key()] = it.value().get<std::string>();
    }
    e.working_dir = j.value("workingDir", "");
    e.role = j.value("role", "");
    e.auto_run = j.value("autoRun", false);
    e.rerun_on_modset_change = j.value("rerunOnModsetChange", false);
    e.requires_virtual_fs_visible =
        j.value("requiresVirtualFsVisible", false);
    if (j.contains("output")) {
        ExecOutput o;
        const auto& oj = j["output"];
        o.path = oj.value("path", "");
        o.arg_name = oj.value("argName", "");
        o.capture = oj.value("capture", "");
        o.synthetic_mod_id = oj.value("syntheticModId", "");
        e.output = std::move(o);
    }
    auto parse_exec_plat = [](const nlohmann::json& j) {
        ExecPlatformOverride po;
        if (j.contains("envVars")) {
            for (auto it = j["envVars"].begin(); it != j["envVars"].end();
                 ++it)
                po.env_vars[it.key()] = it.value().get<std::string>();
        }
        po.launch_options = j.value("launchOptions", "");
        po.proton_version_pin = j.value("protonVersionPin", "");
        if (j.contains("steamOverlay"))
            po.steam_overlay = j["steamOverlay"].get<bool>();
        return po;
    };
    if (j.contains("platform")) {
        const auto& plat = j["platform"];
        if (plat.contains("linux"))
            e.platform.linux_plat = parse_exec_plat(plat["linux"]);
        if (plat.contains("macos"))
            e.platform.macos = parse_exec_plat(plat["macos"]);
        if (plat.contains("windows"))
            e.platform.windows = parse_exec_plat(plat["windows"]);
    }
    return e;
}

PatchEntry parse_patch_entry(const nlohmann::json& j) {
    PatchEntry p;
    p.mod_id = j.value("modId", "");
    if (j.contains("sequence") && !j["sequence"].is_null())
        p.sequence = j["sequence"].get<int>();
    p.target_path = j.value("targetPath", "");
    p.base_file_sha256 = j.value("baseFileSha256", "");
    p.algorithm = j.value("algorithm", "");
    p.payload_base64 = j.value("payloadBase64", "");
    return p;
}

// ---------------------------------------------------------------------------
// Patch filename parsing and chain building
// ---------------------------------------------------------------------------

std::optional<std::pair<std::string, int>> parse_patch_filename(
    const std::string& path) {
    // Expected: "patches/<mod_id>.json" or "patches/<mod_id>-<N>.json"
    const std::string prefix = "patches/";
    if (path.rfind(prefix, 0) != 0) return std::nullopt;

    std::string filename = path.substr(prefix.size());
    const std::string suffix = ".json";
    if (filename.size() <= suffix.size()) return std::nullopt;
    if (filename.substr(filename.size() - suffix.size()) != suffix)
        return std::nullopt;

    filename.resize(filename.size() - suffix.size());
    if (filename.empty()) return std::nullopt;

    // Check for trailing -<N> sequence suffix
    auto dash_pos = filename.rfind('-');
    if (dash_pos != std::string::npos && dash_pos > 0) {
        std::string num_str = filename.substr(dash_pos + 1);
        if (!num_str.empty()) {
            // Verify all digits
            bool all_digits = true;
            for (char c : num_str) {
                if (c < '0' || c > '9') { all_digits = false; break; }
            }
            if (all_digits && !num_str.empty()) {
                int seq = std::stoi(num_str);
                if (seq >= 1) {
                    return std::make_pair(
                        filename.substr(0, dash_pos), seq);
                }
            }
        }
    }

    // No valid sequence suffix - single patch
    return std::make_pair(filename, 0);
}

std::pair<std::vector<PatchChain>, Diagnostics> build_patch_chains(
    const std::vector<PatchEntry>& patches) {
    std::vector<PatchChain> chains;
    Diagnostics diag;

    // Group by mod_id
    std::unordered_map<std::string, std::vector<const PatchEntry*>> groups;
    for (const auto& p : patches) {
        groups[p.mod_id].push_back(&p);
    }

    for (auto& [mod_id, entries] : groups) {
        // Separate sequenced from non-sequenced
        std::vector<PatchEntry> chained;
        std::vector<PatchEntry> unsequenced;
        for (const auto* pe : entries) {
            if (pe->sequence) {
                chained.push_back(*pe);
            } else {
                unsequenced.push_back(*pe);
            }
        }

        // Sort chained patches by sequence ascending
        std::sort(chained.begin(), chained.end(),
                  [](const PatchEntry& a, const PatchEntry& b) {
                      return a.sequence.value_or(0) <
                             b.sequence.value_or(0);
                  });

        // A mod has either one single patch or a chain, never both
        if (!chained.empty() && !unsequenced.empty()) {
            diag.push_back(
                {Diagnostic::Severity::Error,
                 "patches/" + mod_id,
                 "mod has both a single patch and chained patches"});
        }

        // Validate contiguity for chained patches
        for (size_t i = 0; i < chained.size(); ++i) {
            int expected = static_cast<int>(i + 1);
            if (chained[i].sequence.value_or(0) != expected) {
                diag.push_back(
                    {Diagnostic::Severity::Error,
                     "patches/" + mod_id,
                     "non-contiguous sequence: expected " +
                         std::to_string(expected) + ", got " +
                         std::to_string(
                             chained[i].sequence.value_or(0))});
            }
        }

        // Build chain: unsequenced first (single patches), then sorted chained
        PatchChain chain;
        chain.mod_id = mod_id;
        for (auto& p : unsequenced)
            chain.patches.push_back(std::move(p));
        for (auto& p : chained)
            chain.patches.push_back(std::move(p));

        chains.push_back(std::move(chain));
    }

    // Sort chains by mod_id for deterministic output
    std::sort(chains.begin(), chains.end(),
              [](const PatchChain& a, const PatchChain& b) {
                  return a.mod_id < b.mod_id;
              });

    return {std::move(chains), std::move(diag)};
}

// parse_tree() lives in tree_parser.cpp (pure JSON, no archive I/O).
// unpacker.h re-declares it for backward compatibility.

// ---------------------------------------------------------------------------
// Top-level: unpack + validate
// ---------------------------------------------------------------------------

UnpackResult unpack_gmmpack(const std::filesystem::path& archive_path,
                             const std::filesystem::path& schema_dir) {
    UnpackResult result;

    // Load schema set
    SchemaSet schemas = load_schema_set(schema_dir);

    // Stage 1: Extract archive
    auto extract = extract_archive(archive_path);
    if (!extract.ok) {
        result.diagnostics = std::move(extract.diagnostics);
        return result;
    }

    // Reject unknown major versions
    if (extract.manifest_json.contains("gmmpackSchema") &&
        extract.manifest_json["gmmpackSchema"].is_string()) {
        std::string ver = extract.manifest_json["gmmpackSchema"]
                              .get<std::string>();
        auto dot1 = ver.find('.');
        if (dot1 != std::string::npos) {
            int major = 0;
            try {
                major = std::stoi(ver.substr(0, dot1));
            } catch (...) {
                major = -1;
            }
            if (major != 1) {
                result.diagnostics.push_back(
                    {Diagnostic::Severity::Error, "gmmpackSchema",
                     "unsupported major version: " + ver +
                         " (only major version 1 is supported)"});
                result.diagnostics.insert(result.diagnostics.end(),
                                          extract.diagnostics.begin(),
                                          extract.diagnostics.end());
                return result;
            }
        }
    }

    // Stage 1b: Archive integrity
    auto integrity_diag =
        verify_archive_integrity(extract.archive, extract.manifest_json);
    bool integrity_ok = true;
    for (const auto& d : integrity_diag) {
        if (d.severity == Diagnostic::Severity::Error) {
            integrity_ok = false;
            break;
        }
    }
    result.diagnostics.insert(result.diagnostics.end(),
                              integrity_diag.begin(), integrity_diag.end());
    if (!integrity_ok) {
        return result;
    }

    // Stage 2: Schema validation
    auto schema_diag =
        validate_schemas(extract.archive, extract.manifest_json, schemas);
    bool schema_ok = true;
    for (const auto& d : schema_diag) {
        if (d.severity == Diagnostic::Severity::Error) {
            schema_ok = false;
            break;
        }
    }
    result.diagnostics.insert(result.diagnostics.end(), schema_diag.begin(),
                              schema_diag.end());
    if (!schema_ok) {
        return result;
    }

    // Parse manifest
    result.pack.manifest = parse_manifest(extract.manifest_json);

    // Parse all other files
    for (const auto& f : extract.archive.files) {
        if (f.path == "manifest.json" || f.path == "instructions.md") {
            if (f.path == "instructions.md") {
                result.pack.instructions = f.content;
            }
            continue;
        }

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(f.content);
        } catch (...) {
            continue;
        }

        if (f.path.rfind("mods/", 0) == 0) {
            result.pack.mods.push_back(parse_mod_entry(parsed));
        } else if (f.path.rfind("executables/", 0) == 0) {
            result.pack.executables.push_back(parse_executable_entry(parsed));
        } else if (f.path.rfind("patches/", 0) == 0) {
            auto patch = parse_patch_entry(parsed);
            patch.archive_path = f.path;
            result.pack.patches.push_back(std::move(patch));
        } else if (f.path.rfind("ini/", 0) == 0) {
            result.pack.ini_edits.push_back(parse_ini_entry(parsed));
        } else if (f.path == "tree.json") {
            result.pack.tree = parse_tree(parsed);
        }
    }

    // Stage 3: Referential integrity
    auto ref_diag = check_referential_integrity(result.pack);
    result.diagnostics.insert(result.diagnostics.end(), ref_diag.begin(),
                              ref_diag.end());

    // Check for hard errors
    for (const auto& d : result.diagnostics) {
        if (d.severity == Diagnostic::Severity::Error) {
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace engine::gmmpack
