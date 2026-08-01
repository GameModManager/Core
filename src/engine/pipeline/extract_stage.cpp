#include "engine/pipeline/extract_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/model/mod.h"
#include "engine/archive/archive_extractor.h"
#include "engine/log/logger.h"

#include <fstream>
#include <regex>

namespace engine {

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

    // Staging directory for extraction
    auto staging_dir = archive_path.parent_path() / (archive_path.stem().string() + "_tmp");
    std::error_code ec;
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        Logger::instance().error("ExtractStage: failed to create staging dir");
        return false;
    }

    // Extract
    std::vector<ExtractedFile> extracted;
    if (!ArchiveExtractor::extract(archive_path, staging_dir, extracted)) {
        Logger::instance().error("ExtractStage: extraction failed");
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
        // Check edge case: resources/ at staging root means the archive had no wrapper dir
        if (has_resources(staging_dir)) {
            mod_name = archive_path.stem().string();
            mod.id = mod_name;
        }
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
            // Flatten: move staging/{prefix}/* → staging/ so InstallStage doesn't double-nest
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
