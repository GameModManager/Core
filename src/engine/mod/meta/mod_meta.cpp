#include "engine/mod/meta/mod_meta.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace engine {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// ---------------------------------------------------------------------------
// INI value escaping
// ---------------------------------------------------------------------------
//
// The INI format has no native way to embed a literal newline inside a value:
// any newline is a record separator, not part of the value. Raw Nexus / Steam
// descriptions are multi-line BBCode, so we MUST encode newlines when writing
// and decode them when reading, otherwise:
//   - serialize() injects a literal newline mid-value, breaking INI structure
//     (anything after the first newline is re-parsed as a new section header
//     / key-value / comment, silently corrupting subsequent data)
//   - On re-parse, the value is truncated at the first newline and the rest
//     leaks into other keys / sections. Nexus descriptions are typically
//     several paragraphs of BBCode, so almost every fetch would lose data.
//
// Convention: backslash-escape a small set of control characters. We use
// the same two-character sequences as JSON-ish strings:
//   '\n' (LF)   -> "\\n"
//   '\r' (CR)   -> "\\r"
//   '\\' (literal backslash) -> "\\\\"
// All other bytes are emitted verbatim. Order matters: encode the backslash
// FIRST so a value containing "\n" becomes "\\\\n" (one literal backslash
// followed by 'n'), not a real newline. This is intentionally minimal - we
// don't need to escape " or ' or anything else that the INI grammar accepts
// in values.
static std::string ini_escape_value(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static std::string ini_unescape_value(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char next = v[i + 1];
            switch (next) {
                case 'n':  out += '\n'; ++i; continue;
                case 'r':  out += '\r'; ++i; continue;
                case '\\': out += '\\'; ++i; continue;
                default:   break;  // unknown escape: keep the backslash
            }
        }
        out += v[i];
    }
    return out;
}

static std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

// MO2 Nexus-specific fields that get moved from [General] to [Nexusmods]
static const std::unordered_set<std::string> kNexusFields = {
    "modid", "nexusfilestatus", "lastnexusquery", "lastnexusupdate",
    "nexuslastmodified", "nexuscategory", "nexusdescription",
};

// Fields we drop from MO2's [General] (engine-internal, no value to us)
static const std::unordered_set<std::string> kDropFields = {
    "gamename", "repository",
};

// ---------------------------------------------------------------------------
// Section management
// ---------------------------------------------------------------------------

static int section_index(const std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>& sections,
                         const std::string& name) {
    for (int i = 0; i < static_cast<int>(sections.size()); ++i) {
        if (sections[i].first == name) return i;
    }
    return -1;
}

// Ensure a section exists (return its index, create if needed at end)
static int ensure_section(std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>>& sections,
                          const std::string& name) {
    int idx = section_index(sections, name);
    if (idx < 0) {
        idx = static_cast<int>(sections.size());
        sections.emplace_back(name, std::unordered_map<std::string, std::string>{});
    }
    return idx;
}

// ---------------------------------------------------------------------------
// ModMeta implementation
// ---------------------------------------------------------------------------

std::string ModMeta::get(const std::string& section, const std::string& key) const {
    int idx = section_index(sections_, section);
    if (idx < 0) return {};
    auto it = sections_[idx].second.find(key);
    if (it == sections_[idx].second.end()) return {};
    return it->second;
}

void ModMeta::set(const std::string& section, const std::string& key,
                  const std::string& value) {
    int idx = ensure_section(sections_, section);
    sections_[idx].second[key] = value;
}

bool ModMeta::has_section(const std::string& section) const {
    return section_index(sections_, section) >= 0;
}

std::vector<std::string> ModMeta::sections() const {
    std::vector<std::string> out;
    out.reserve(sections_.size());
    for (const auto& [name, _] : sections_) {
        out.push_back(name);
    }
    return out;
}

std::vector<std::string> ModMeta::keys(const std::string& section) const {
    std::vector<std::string> out;
    int idx = section_index(sections_, section);
    if (idx < 0) return out;
    for (const auto& [k, _] : sections_[idx].second) {
        out.push_back(k);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------

std::string ModMeta::serialize() const {
    std::ostringstream out;
    bool first = true;
    for (const auto& [section, kv] : sections_) {
        if (!first) out << "\n";
        first = false;
        out << "[" << section << "]\n";
        for (const auto& [key, value] : kv) {
            // Escape control bytes that would otherwise corrupt the INI
            // structure. A raw '\n' would end the value mid-line, so the
            // parser re-reads the rest as a new section / key, silently
            // losing data. See ini_escape_value() above for the encoding.
            out << key << " = " << ini_escape_value(value) << "\n";
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Parse INI
// ---------------------------------------------------------------------------

bool ModMeta::parse(const std::string& content) {
    sections_.clear();

    std::istringstream in(content);
    std::string line;
    std::string current_section;

    while (std::getline(in, line)) {
        auto trimmed_line = trim(line);
        if (trimmed_line.empty()) continue;

        // Comment
        if (trimmed_line[0] == ';' || trimmed_line[0] == '#') continue;

        // Section header
        if (trimmed_line[0] == '[') {
            auto close = trimmed_line.find(']');
            if (close == std::string::npos) return false; // malformed
            current_section = trim(trimmed_line.substr(1, close - 1));
            if (current_section.empty()) return false; // empty section name
            ensure_section(sections_, current_section);
            continue;
        }

        // Key-value
        auto eq = trimmed_line.find('=');
        if (eq == std::string::npos) continue; // no '=' → skip

        if (current_section.empty()) return false; // value outside section

        auto key = trim(trimmed_line.substr(0, eq));
        auto val = trim(trimmed_line.substr(eq + 1));
        if (key.empty()) continue;

        // Reverse the encode applied in serialize(): backslash-escaped
        // control bytes (\\n, \\r, \\\\) in the raw text become real
        // bytes here. Without this round-trip, every multi-line value
        // (e.g. raw Nexus / Steam BBCode descriptions) would come back
        // truncated at the first literal newline.
        val = ini_unescape_value(val);

        int idx = ensure_section(sections_, current_section);
        sections_[idx].second[key] = val;
    }

    return true;
}

// ---------------------------------------------------------------------------
// From MO2 import
// ---------------------------------------------------------------------------

ModMeta ModMeta::from_mo2_import(const std::string& content,
                                 const std::string& folder_name) {
    ModMeta raw;
    raw.parse(content);

    ModMeta meta;

    // Detect source
    std::string repository = raw.get("General", "repository");
    bool is_nexus = (repository == "Nexus");

    // Copy [General] fields, splitting Nexus-specific ones
    std::string source_type = "manual";
    std::string source_id;

    if (raw.has_section("General")) {
        // Determine key ordering: generic first, MO2-origin generic, then nexus fields
        // We copy non-nexus, non-dropped fields to our [General]
        for (const auto& [key, value] : raw.sections_[section_index(raw.sections_, "General")].second) {
            if (kDropFields.count(key)) continue;

            if (is_nexus && kNexusFields.count(key)) {
                meta.set("Nexusmods", key, value);
                if (key == "modid") {
                    meta.set("Nexusmods", "mod_id", value); // alias for clarity
                    source_id = value;
                }
                continue;
            }

            meta.set("General", key, value);
        }

        if (is_nexus) {
            source_type = "nexus";
            // Store the MO2 gamename for reference
            std::string gamename = raw.get("General", "gamename");
            if (!gamename.empty()) {
                meta.set("GameModManager", "mo2_gamename", gamename);
            }
        }
    }

    // Copy [installedFiles] into [Nexusmods] as installedFiles\key
    if (is_nexus && raw.has_section("installedFiles")) {
        for (const auto& [key, value] : raw.sections_[section_index(raw.sections_, "installedFiles")].second) {
            meta.set("Nexusmods", "installedFiles\\" + key, value);
            // Extract primary fileid from 1\fileid
            if (key == "1\\fileid" && source_id.empty()) {
                // source_id already set from modid above, but store fileid separately
                meta.set("Nexusmods", "fileid", value);
            }
        }
        // Also promote fileid to top-level Nexusmods key if we have it
        std::string f1_id = raw.get("installedFiles", "1\\fileid");
        if (!f1_id.empty()) {
            meta.set("Nexusmods", "fileid", f1_id);
        }
    }

    // --- [GameModManager] ---
    meta.set("GameModManager", "folder", folder_name);
    meta.set("GameModManager", "source_type", source_type);
    meta.set("GameModManager", "source_id", source_id);
    meta.set("GameModManager", "imported_from_mo2", "true");
    meta.set("GameModManager", "original_meta_path", folder_name + "/meta.ini");
    meta.set_meta_version(CURRENT_META_VERSION);

    return meta;
}

// ---------------------------------------------------------------------------
// From default (fresh mod, no MO2)
// ---------------------------------------------------------------------------

ModMeta ModMeta::from_default(const std::string& folder_name,
                              const std::string& source_type,
                              const std::string& source_id,
                              const std::string& installation_file,
                              const std::string& version) {
    ModMeta meta;

    if (!version.empty()) meta.set("General", "version", version);
    if (!installation_file.empty()) meta.set("General", "installationfile", installation_file);
    meta.set("General", "installed", current_timestamp());

    meta.set("GameModManager", "folder", folder_name);
    meta.set("GameModManager", "source_type", source_type);
    meta.set("GameModManager", "source_id", source_id);
    meta.set("GameModManager", "imported_from_mo2", "false");
    meta.set_meta_version(CURRENT_META_VERSION);

    return meta;
}

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

int ModMeta::meta_version() const {
    auto v = get("GameModManager", "meta_version");
    if (v.empty()) return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

void ModMeta::set_meta_version(int v) {
    set("GameModManager", "meta_version", std::to_string(v));
}

// ---------------------------------------------------------------------------
// Convenience accessors
// ---------------------------------------------------------------------------

std::string ModMeta::folder() const {
    return get("GameModManager", "folder");
}

std::string ModMeta::source_type() const {
    return get("GameModManager", "source_type");
}

std::string ModMeta::source_id() const {
    return get("GameModManager", "source_id");
}

std::string ModMeta::source_page_url() const {
    // Provider sections are arbitrary; check the known ones that persist a
    // page link. LoversLab is the only source today that cannot reconstruct
    // its page from source_id alone (the slug lives in the download link).
    if (has_section("LoversLab")) {
        auto url = get("LoversLab", "page_url");
        if (!url.empty()) return url;
    }
    return {};
}

std::string ModMeta::separator_id() const {
    return get("GameModManager", "separator_id");
}

void ModMeta::set_separator_id(const std::string& id) {
    set("GameModManager", "separator_id", id);
}

std::string ModMeta::version() const {
    return get("General", "version");
}

int ModMeta::priority() const {
    auto v = get("GameModManager", "priority");
    if (v.empty()) return -1;
    try { return std::stoi(v); } catch (...) { return -1; }
}

void ModMeta::set_priority(int p) {
    set("GameModManager", "priority", std::to_string(p));
}

bool ModMeta::imported_from_mo2() const {
    return get("GameModManager", "imported_from_mo2") == "true";
}

// ---------------------------------------------------------------------------
// Per-mod UI state (manager sidecar)
// ---------------------------------------------------------------------------

bool ModMeta::folded() const {
    return get("GameModManager", "folded") == "true";
}

void ModMeta::set_folded(bool folded_state) {
    set("GameModManager", "folded", folded_state ? "true" : "false");
}

std::string ModMeta::parent_id() const {
    return get("GameModManager", "parent_id");
}

void ModMeta::set_parent_id(const std::string& id) {
    set("GameModManager", "parent_id", id);
}

void ModMeta::unset(const std::string& section, const std::string& key) {
    int idx = section_index(sections_, section);
    if (idx < 0) return;
    sections_[idx].second.erase(key);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

ModMeta ModMeta::load(const std::filesystem::path& meta_dir,
                      const std::string& folder_name) {
    ModMeta meta;
    auto filepath = meta_dir / (folder_name + ".ini");
    std::ifstream f(filepath);
    if (!f) return meta; // empty meta - caller checks has_section("General")

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    meta.parse(content);
    return meta;
}

bool ModMeta::save(const std::filesystem::path& meta_dir,
                   const std::string& folder_name) const {
    std::error_code ec;
    std::filesystem::create_directories(meta_dir, ec);
    if (ec) return false;

    auto filepath = meta_dir / (folder_name + ".ini");
    std::ofstream f(filepath);
    if (!f) return false;
    f << serialize();
    return f.good();
}

ModMeta ModMeta::load_file(const std::filesystem::path& ini_file) {
    ModMeta meta;
    std::ifstream f(ini_file);
    if (!f) return meta;  // empty meta
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    meta.parse(content);
    return meta;
}

bool ModMeta::save_file(const std::filesystem::path& ini_file) const {
    std::error_code ec;
    std::filesystem::create_directories(ini_file.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(ini_file);
    if (!f) return false;
    f << serialize();
    return f.good();
}

bool ModMeta::exists(const std::filesystem::path& meta_dir,
                     const std::string& folder_name) {
    return std::filesystem::exists(meta_dir / (folder_name + ".ini"));
}

// ---------------------------------------------------------------------------
// MO2 detection
// ---------------------------------------------------------------------------

bool ModMeta::has_mo2_meta(const std::filesystem::path& mod_folder) {
    return std::filesystem::exists(mod_folder / "meta.ini");
}

ModMeta ModMeta::import_mo2(const std::filesystem::path& mod_folder,
                            const std::string& folder_name) {
    auto meta_path = mod_folder / "meta.ini";
    std::ifstream f(meta_path);
    if (!f) return {};

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    return from_mo2_import(content, folder_name);
}

// ---------------------------------------------------------------------------
// Game-visible metadata files
// ---------------------------------------------------------------------------

bool ModMeta::write_game_metadata(const std::filesystem::path& mod_dir,
                                  const std::string& metadata_file,
                                  const std::string& display_name,
                                  const std::string& version,
                                  const std::string& nexus_mod_id,
                                  const std::string& installation_file) {
    if (mod_dir.empty()) return false;

    if (metadata_file.empty() || metadata_file == "meta.ini") {
        auto meta_ini = mod_dir / "meta.ini";
        if (std::filesystem::exists(meta_ini)) return true;
        std::ofstream mf(meta_ini);
        if (!mf) return false;
        // `nexus_mod_id` is the Nexus mod id when the mod came from Nexus
        // and an empty string for every other source (Steam workshop id,
        // LoversLab file id, manual archive, ...). The MO2 meta.ini modid=
        // field is the Nexus id namespace specifically; writing a
        // non-Nexus id here makes any MO2-style reader (and our own
        // importers) treat the mod as Nexus-valid. Empty -> "0", the
        // MO2 sentinel for "no Nexus id", so manual / Steam / LoversLab
        // mods do NOT inherit a fake Nexus attribution.
        mf << "[General]\n";
        mf << "modid=" << (nexus_mod_id.empty() ? "0" : nexus_mod_id) << "\n";
        mf << "version=" << (version.empty() ? "1.0" : version) << "\n";
        mf << "newestVersion=" << (version.empty() ? "1.0" : version) << "\n";
        mf << "category=0\n";
        if (!installation_file.empty())
            mf << "installationFile=" << installation_file << "\n";
        return mf.good();
    }

    auto metadata_path = mod_dir / metadata_file;
    if (std::filesystem::exists(metadata_path)) return true;
    std::ofstream mf(metadata_path);
    if (!mf) return false;
    std::string name = display_name.empty() ? mod_dir.filename().string()
                                            : display_name;
    mf << "<?xml version=\"1.0\"?>\n"
       << "<mod>\n"
       << "  <name>" << name << "</name>\n"
       << "  <version>" << (version.empty() ? "1.0" : version) << "</version>\n"
       << "</mod>\n";
    return mf.good();
}

} // namespace engine
