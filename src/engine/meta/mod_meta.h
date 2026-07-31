#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

namespace engine {

// Section-based metadata for a single mod.
// Format: [General] + [{provider}] sections + [GameModManager].
// Provider sections are arbitrary - any game/plugin can write its own.
class ModMeta {
public:
    std::string get(const std::string& section, const std::string& key) const;
    void set(const std::string& section, const std::string& key,
             const std::string& value);

    bool has_section(const std::string& section) const;
    std::vector<std::string> sections() const;
    std::vector<std::string> keys(const std::string& section) const;

    // Serialize/parse INI format
    std::string serialize() const;
    bool parse(const std::string& content);

    // --- Factories ---

    // Import from an MO2 meta.ini found inside a mod folder.
    // Detects repository=Nexus → moves nexus fields to [Nexusmods],
    // copies [installedFiles] as subkeys under [Nexusmods].
    static ModMeta from_mo2_import(const std::string& content,
                                   const std::string& folder_name);

    // Create fresh meta for a newly added mod (no MO2 history).
    static ModMeta from_default(const std::string& folder_name,
                                const std::string& source_type,
                                const std::string& source_id,
                                const std::string& installation_file = {},
                                const std::string& version = {});

    // --- Convenience accessors ---

    [[nodiscard]] std::string folder() const;
    [[nodiscard]] std::string source_type() const;
    [[nodiscard]] std::string source_id() const;
    [[nodiscard]] std::string separator_id() const;
    void set_separator_id(const std::string& id);
    [[nodiscard]] std::string version() const;
    [[nodiscard]] int priority() const;
    void set_priority(int p);
    [[nodiscard]] bool imported_from_mo2() const;

    // --- File I/O ---
    // Load/save meta file at {instance_root}/meta/{folder_name}.ini
    static ModMeta load(const std::filesystem::path& meta_dir,
                        const std::string& folder_name);
    bool save(const std::filesystem::path& meta_dir,
              const std::string& folder_name) const;

    // Check if a meta file already exists
    static bool exists(const std::filesystem::path& meta_dir,
                       const std::string& folder_name);

    // --- MO2 detection ---
    static bool has_mo2_meta(const std::filesystem::path& mod_folder);
    static ModMeta import_mo2(const std::filesystem::path& mod_folder,
                              const std::string& folder_name);

    // --- Versioning ---
    // Increment this whenever the meta format changes (new sections/keys added).
    // Existing meta files with an older version get upgraded on next load.
    static constexpr int CURRENT_META_VERSION = 1;

    // Returns meta_version from [GameModManager], or 0 if absent (pre-versioning).
    [[nodiscard]] int meta_version() const;
    void set_meta_version(int v);

private:
    // Ordered sections for deterministic serialization.
    // Section order: General → (provider sections, insertion order) → GameModManager
    std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>> sections_;
};

} // namespace engine
