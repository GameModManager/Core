#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/model/mod.h"
#include "engine/archive/archive_extractor.h"
#include "engine/fomod/fomod_utils.h"
#include "engine/log/logger.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <unordered_set>

namespace engine {

namespace {

// Data-dir folder names the MO2 GamebryoModDataChecker accepts as proof the
// root IS the game's data dir (gamebryomoddatachecker.cpp possibleFolderNames),
// plus "source" (a real Skyrim Data folder contains the CK sources). Entries
// are matched case-insensitively.
const std::unordered_set<std::string>& data_dir_folders() {
    static const std::unordered_set<std::string> s = {
        "fonts", "interface", "menus", "meshes", "music", "scripts", "shaders",
        "sound", "strings", "textures", "trees", "video", "facegen", "materials",
        "skse", "obse", "mwse", "nvse", "fose", "f4se", "distantlod", "asi",
        "skyproc patchers", "tools", "mcm", "icons", "bookart", "distantland",
        "mits", "splash", "dllplugins", "calientetools", "netscriptframework",
        "shadersfx", "source",
    };
    return s;
}

// Data-dir file extensions the MO2 checker accepts (possibleFileExtensions).
const std::unordered_set<std::string>& data_dir_extensions() {
    static const std::unordered_set<std::string> s = {
        "esp", "esm", "esl", "bsa", "ba2", "modgroups", "ini",
    };
    return s;
}

// MO2's isDataTextArchiveTopLayer: exactly one directory named data_folder_name
// plus one or more "useless" files (text/pdf/md/images) and nothing else.
bool is_data_text_top_layer(const std::filesystem::path& root,
                            const std::string& data_folder_name) {
    static const std::unordered_set<std::string> junk = {
        "txt", "pdf", "md", "jpg", "jpeg", "png", "bmp",
    };
    bool data_found = false;
    bool txt_found = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) return false;
        if (entry.is_directory()) {
            if (data_found ||
                !name_matches_ci(entry.path(), data_folder_name)) {
                return false;
            }
            data_found = true;
        } else if (entry.is_regular_file()) {
            auto ext = toLower(entry.path().extension().string());
            if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
            if (!junk.count(ext)) return false;
            txt_found = true;
        }
    }
    return data_found && txt_found;
}

}  // namespace

StagingNormalizeResult normalize_staging_root(
    const std::filesystem::path& staging_root,
    const std::string& data_folder_name) {
    StagingNormalizeResult result;
    if (data_folder_name.empty()) return result;

    std::filesystem::path current = staging_root;
    std::error_code ec;

    while (true) {
        // A FOMOD archive owns its own layout (and its own name wizard) -
        // never reshape it. find_fomod_dir already descends single-dir
        // wrappers, so an archive like <wrapper>/fomod/ModuleConfig.xml is
        // caught here too.
        if (find_fomod_dir(current)) {
            result.fomod = true;
            return result;
        }

        // Root already looks like the game's data dir? It's the base.
        bool data_looks_valid = false;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
            if (ec) break;
            if (entry.is_directory()) {
                if (data_dir_folders().count(toLower(entry.path().filename().string()))) {
                    data_looks_valid = true;
                    break;
                }
            } else if (entry.is_regular_file()) {
                auto ext = toLower(entry.path().extension().string());
                if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
                if (data_dir_extensions().count(ext)) {
                    data_looks_valid = true;
                    break;
                }
            }
        }
        if (data_looks_valid) {
            result.simple = true;
            return result;
        }

        // DataText top layer: a lone "Data" wrapper around data + readmes.
        if (is_data_text_top_layer(current, data_folder_name)) {
            result.simple = true;
            result.merged_data_dir = true;
            // Merge <root>/Data/* up into <root> (MO2's install() detaches the
            // data dir and merges it into the tree root).
            std::filesystem::path data_dir;
            for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
                if (ec) break;
                if (entry.is_directory() && name_matches_ci(entry.path(), data_folder_name)) {
                    data_dir = entry.path();
                    break;
                }
            }
            if (!data_dir.empty()) {
                for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
                    if (ec) break;
                    auto dest = current / entry.path().filename();
                    std::filesystem::rename(entry.path(), dest, ec);
                    if (ec) break;
                }
                std::filesystem::remove_all(data_dir, ec);
            }
            return result;
        }

        // Exactly one entry and it's a directory: peel the wrapper and retry.
        std::vector<std::filesystem::path> children;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
            if (ec) break;
            children.push_back(entry.path());
        }
        if (ec || children.size() != 1 || !std::filesystem::is_directory(children[0], ec)) {
            return result;  // not simple - install the root as-is
        }

        if (result.peeled_folder_hint.empty()) {
            result.peeled_folder_hint = children[0].filename().string();
        }
        auto wrapper = children[0];
        for (const auto& entry : std::filesystem::directory_iterator(wrapper, ec)) {
            if (ec) break;
            auto dest = current / entry.path().filename();
            std::filesystem::rename(entry.path(), dest, ec);
            if (ec) break;
        }
        std::filesystem::remove_all(wrapper, ec);
    }
}

// Simple XML tag extraction (no XML library dependency)
static std::string xml_find_tag(const std::string& xml, const std::string& tag) {
    auto open = "<" + tag + ">";
    auto close = "</" + tag + ">";
    auto pos = xml.find(open);
    if (pos == std::string::npos) return {};
    pos += open.size();
    auto end = xml.find(close, pos);
    if (end == std::string::npos) return {};
    auto content = xml.substr(pos, end - pos);
    auto first = content.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return {};
    auto last = content.find_last_not_of(" \t\n\r");
    return content.substr(first, last - first + 1);
}

// Try to read a text file, return empty if missing
static std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

bool ExtractStage::execute(Mod& mod, PipelineContext& ctx) {
    if (mod.files.empty() || mod.files[0].relative_path.empty()) {
        // Metadata-only mods (e.g. Steam Workshop already on disk) - nothing to extract
        if (mod.state == ModState::Downloaded) {
            mod.state = ModState::Extracted;
            return true;
        }
        Logger::instance().error("ExtractStage: no archive path in mod files");
        return false;
    }

    auto archive_path = std::filesystem::path(mod.files[0].relative_path);
    if (!std::filesystem::exists(archive_path)) {
        Logger::instance().error("ExtractStage: archive not found: " + archive_path.string());
        return false;
    }

    // Staging directory for extraction. Lives OUTSIDE the watched downloads
    // dir - a hidden sibling of the mods dir - so the Downloads tab's directory
    // watchdog never fires on every install (and the <archive>_tmp leaks from
    // older builds never re-appear there). Falls back to next-to-the-archive
    // when no mods dir is in context (headless). Pipeline::run removes it on
    // failure/cancel.
    auto staging_dir = ctx.mods_dir.empty()
        ? (archive_path.parent_path() / (archive_path.stem().string() + "_tmp"))
        : (ctx.mods_dir.parent_path() / ".gmm_install_tmp");
    std::error_code ec;
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        Logger::instance().error("ExtractStage: failed to create staging dir " +
                                 staging_dir.string() + ": " + ec.message());
        return false;
    }

    // Extract (with progress): the extractor sums the archive's uncompressed
    // size in a header pre-pass, then reports bytes written against it, so the
    // bar shows a real percent. A size that could not be determined (-1) falls
    // back to an indeterminate stage.
    std::vector<ExtractedFile> extracted;
    std::string extract_error;
    const std::string extract_status = "Extracting " + archive_path.filename().string() + "…";
    const bool extracted_ok = ArchiveExtractor::extract(
        archive_path, staging_dir, extracted, extract_error,
        [&ctx, &extract_status](int64_t done, int64_t total) {
            if (!ctx.on_stage_progress) return;
            if (total <= 0) {
                ctx.on_stage_progress(-1, extract_status);
                return;
            }
            const int pct = static_cast<int>(done * 100 / total);
            ctx.on_stage_progress(std::clamp(pct, 0, 100), extract_status);
        });
    if (!extracted_ok) {
        Logger::instance().error("ExtractStage: extraction failed for " +
                                 archive_path.string() + " into " +
                                 staging_dir.string() + ": " + extract_error);
        std::filesystem::remove_all(staging_dir, ec);
        return false;
    }

    Logger::instance().debug("ExtractStage: extracted " +
                            std::to_string(extracted.size()) +
                            " files to " + staging_dir.string());

    // Determine mod folder name and metadata
    // First try: if archive contains a top-level folder with metadata.xml, use that name
    // Second try: check for Isaac-style mods (resources/ or resources-dlc3/ at root)
    // Third try: use common parent dir as the mod name
    // Fallback: use archive stem

    std::string mod_name;
    std::string mod_version;

    // Check for the game's metadata file at the top level of extracted content.
    // Only XML-metadata games (Isaac) derive the display name from it; for
    // MO2-style games the mod name comes from the folder structure below and
    // the archive stem - never from metadata.xml.
    if (!ctx.metadata_file.empty() && ctx.metadata_file != "meta.ini") {
        auto meta_xml = staging_dir / ctx.metadata_file;
        if (std::filesystem::exists(meta_xml)) {
            auto content = read_file_text(meta_xml);
            if (!content.empty()) {
                mod_name = xml_find_tag(content, "name");
                mod_version = xml_find_tag(content, "version");
            }
        }
    }

    // Check for directories that indicate Isaac mods or similar structure
    auto has_resources = [](const std::filesystem::path& dir) {
        return std::filesystem::exists(dir / "resources") ||
               std::filesystem::exists(dir / "resources-dlc3") ||
               std::filesystem::exists(dir / "resources-dlc4") ||
               std::filesystem::exists(dir / "resources-dlc5") ||
               std::filesystem::exists(dir / "mod.asm");
    };

    if (mod_name.empty()) {
        // Isaac-style games (deploy_include_mod_id=true) ship the mod's own
        // folder in the archive - the wrapper IS the mod name, so it is kept
        // as-is and used for naming, matching how GMM always handled them.
        if (ctx.deploy_include_mod_id) {
            // Check edge case: resources/ at staging root means the archive had
            // no wrapper dir
            if (has_resources(staging_dir)) {
                mod_name = archive_path.stem().string();
                mod.id = mod_name;
            }
            // If no name yet, check if extracted files have a common parent dir
            if (mod_name.empty() && !extracted.empty()) {
                std::string common_prefix;
                for (const auto& ef : extracted) {
                    auto path = ef.dest_path;
                    auto rel = std::filesystem::relative(path, staging_dir);
                    if (rel.empty()) continue;
                    auto first_component = (*rel.begin()).string();
                    if (common_prefix.empty()) {
                        common_prefix = first_component;
                    } else if (common_prefix != first_component) {
                        common_prefix.clear();
                        break;
                    }
                }
                if (!common_prefix.empty()) {
                    mod_name = common_prefix;
                    mod.id = mod_name;
                    // Flatten: move staging/{prefix}/* → staging/ so
                    // InstallStage doesn't double-nest
                    auto prefix_dir = staging_dir / common_prefix;
                    if (std::filesystem::exists(prefix_dir)) {
                        std::error_code ec2;
                        for (auto& entry : std::filesystem::directory_iterator(prefix_dir)) {
                            auto dest = staging_dir / entry.path().filename();
                            std::filesystem::rename(entry.path(), dest, ec2);
                        }
                        std::filesystem::remove_all(prefix_dir, ec2);
                    }
                }
            }
        } else {
            // MO2-style games (Skyrim et al.): normalize the archive root so a
            // wrapper like "SKSE" is not stripped as if it were "Data", and a
            // lone "Data" wrapper gets unwrapped. FOMOD archives are skipped
            // (FomodStage owns them). The first peeled wrapper name is a better
            // name hint than the archive stem unless it's the data dir itself.
            auto normalized = normalize_staging_root(staging_dir, ctx.deploy_prefix);
            if (!normalized.fomod && !normalized.peeled_folder_hint.empty() &&
                !name_matches_ci(normalized.peeled_folder_hint, ctx.deploy_prefix)) {
                mod_name = normalized.peeled_folder_hint;
                mod.id = mod_name;
            }
        }
    }

    if (mod_name.empty()) {
        mod_name = archive_path.stem().string();
        mod.id = mod_name;
    }

    if (mod.name.empty()) mod.name = mod_name;
    if (!mod_version.empty()) mod.version = mod_version;

    // Store staging path - InstallStage will move to mods/
    ModFile staging_entry;
    staging_entry.relative_path = staging_dir.string();
    mod.files.push_back(staging_entry);

    mod.state = ModState::Extracted;
    return true;
}

} // namespace engine
