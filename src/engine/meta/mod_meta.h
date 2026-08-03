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
    // Load/save meta file at {meta_dir}/{folder_name}.ini
    static ModMeta load(const std::filesystem::path& meta_dir,
                        const std::string& folder_name);
    bool save(const std::filesystem::path& meta_dir,
              const std::string& folder_name) const;

    // Load/save meta at an explicit .ini path (e.g. a mod's own meta.ini,
    // which lives inside the mod folder rather than the manager sidecar).
    static ModMeta load_file(const std::filesystem::path& ini_file);
    bool save_file(const std::filesystem::path& ini_file) const;

    // Check if a meta file already exists
    static bool exists(const std::filesystem::path& meta_dir,
                       const std::string& folder_name);

    // --- MO2 detection ---
    static bool has_mo2_meta(const std::filesystem::path& mod_folder);
    static ModMeta import_mo2(const std::filesystem::path& mod_folder,
                              const std::string& folder_name);

    // --- Game-visible metadata files ---
    // Write the game's metadata file into a mod folder so ModScanner
    // recognizes the mod. MO2-style games get a meta.ini ([General] with
    // modid/version/newestVersion/category/installationFile - the same file
    // MO2's installers write); games that registered the metadata_file hook
    // (Isaac) get their XML metadata file. No-op if the file already exists.
    // Returns true on success (or if it already existed).
    static bool write_game_metadata(const std::filesystem::path& mod_dir,
                                    const std::string& metadata_file,
                                    const std::string& display_name,
                                    const std::string& version,
                                    const std::string& modid = {},
                                    const std::string& installation_file = {});

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
