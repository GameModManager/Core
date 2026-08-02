#include "engine/detect/mod_scanner.h"
#include "engine/registry/game_knowledge.h"
#include "engine/log/logger.h"
#include "engine/meta/mod_meta.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <regex>

namespace engine {

// --- Simple XML tag extraction (no libxml/Qt dependency) ---

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

static std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// --- Helpers ---

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        // trim
        auto start = token.find_first_not_of(" \t");
        auto end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            result.push_back(token.substr(start, end - start + 1));
        }
    }
    return result;
}

static bool should_ignore(const std::string& name,
                          const std::vector<std::string>& ignored) {
    for (const auto& ig : ignored) {
        if (name == ig) return true;
    }
    return false;
}

// --- ModScanner ---

// Shared implementation: scan the given mods_dir for mods.
static std::vector<ScannedMod> scan_impl(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& mods_dir,
    const std::vector<std::filesystem::path>& ignore_symlink_targets) {

    // Read all config from GameKnowledge hooks
    auto disable_file = knowledge.get(game_id, "disable_mechanism", "");
    auto ignored_csv  = knowledge.get(game_id, "ignored_files", "");
    // MO2-style metadata is a meta.ini in the mod folder. Games whose engine
    // reads XML metadata from mod folders (Isaac's metadata.xml) register the
    // filename and name/version tags via hooks.
    auto metadata_file = knowledge.get(game_id, "metadata_file", "meta.ini");
    auto name_tag     = knowledge.get(game_id, "metadata_name_tag", "name");
    auto version_tag  = knowledge.get(game_id, "metadata_version_tag", "version");
    bool use_xml_meta = !metadata_file.empty() && metadata_file != "meta.ini";
    auto separator_suffix = knowledge.get(game_id, "separator_suffix", "_separator");
    auto workshop_pattern = knowledge.get(game_id, "workshop_id_pattern", "");

    auto ignored = split_csv(ignored_csv);
    // Always ignore system directories during directory scanning
    ignored.emplace_back("overwrite");
    if (!metadata_file.empty() && should_ignore(metadata_file, ignored) == false) {
        ignored.push_back(metadata_file);
    }
    if (!disable_file.empty() && should_ignore(disable_file, ignored) == false) {
        ignored.push_back(disable_file);
    }

    std::vector<ScannedMod> mods;
    std::error_code ec;

    if (!std::filesystem::exists(mods_dir)) {
        Logger::instance().warn("ModScanner: mods directory not found: " + mods_dir.string());
        return mods;
    }

    for (const auto& entry : std::filesystem::directory_iterator(mods_dir)) {
        try {
            if (!entry.is_directory()) continue;
        } catch (const std::filesystem::filesystem_error&) {
            continue;
        }

        auto folder_name = entry.path().filename().string();
        if (should_ignore(folder_name, ignored)) continue;

        // Skip directories that are symlinks to paths we manage (e.g. Overwrite)
        if (!ignore_symlink_targets.empty()) {
            std::error_code ec2;
            if (std::filesystem::is_symlink(entry.path(), ec2)) {
                auto link_target = std::filesystem::read_symlink(entry.path(), ec2);
                if (!ec2) {
                    if (link_target.is_relative())
                        link_target = std::filesystem::absolute(entry.path().parent_path() / link_target);
                    auto resolved = std::filesystem::weakly_canonical(link_target, ec2);
                    if (!ec2) {
                        bool skip = false;
                        for (const auto& ignore_root : ignore_symlink_targets) {
                            auto canon_root = std::filesystem::weakly_canonical(ignore_root, ec2);
                            if (!ec2) {
                                auto r_str = resolved.string();
                                auto i_str = canon_root.string();
                                if (r_str.size() >= i_str.size() &&
                                    r_str.compare(0, i_str.size(), i_str) == 0) {
                                    skip = true;
                                    break;
                                }
                            }
                        }
                        if (skip) continue;
                    }
                }
            }
        }

        ScannedMod mod;
        mod.folder_name = folder_name;

        // Extract workshop ID from folder name if pattern is configured
        if (!workshop_pattern.empty()) {
            try {
                std::regex ws_re(workshop_pattern);
                std::smatch m;
                if (std::regex_search(folder_name, m, ws_re)) {
                    mod.workshop_id = std::stoll(m[1].str());
                }
            } catch (...) {}
        }

        // Check for separator (game-specific suffix)
        if (!separator_suffix.empty() &&
            folder_name.size() > separator_suffix.size() &&
            folder_name.compare(folder_name.size() - separator_suffix.size(),
                                separator_suffix.size(),
                                separator_suffix) == 0) {
            mod.is_separator = true;

            // MO2-style separator: the folder is "<name>_separator" and the
            // display name is the folder minus the suffix (ModList::getDisplayName).
            // An optional color comes from the meta.ini [General] color key -
            // the same file MO2's setColor writes. No color means no color.
            mod.display_name = folder_name.substr(0, folder_name.size() - separator_suffix.size());
            auto meta_path = entry.path() / "meta.ini";
            auto meta_content = read_file_text(meta_path);
            if (!meta_content.empty()) {
                engine::ModMeta meta;
                if (meta.parse(meta_content)) {
                    mod.separator_color = meta.get("General", "color");
                }
            }
            mod.raw_name = folder_name;
            mods.push_back(std::move(mod));
            continue;
        }

        // Parse metadata file
        if (use_xml_meta) {
            // XML metadata (Isaac): the mod is only recognized if the file
            // exists; name/version come from the configured tags.
            auto metadata_path = entry.path() / metadata_file;
            if (!std::filesystem::exists(metadata_path)) continue;

            auto content = read_file_text(metadata_path);
            if (content.empty()) continue;

            auto raw_name = xml_find_tag(content, name_tag);
            if (raw_name.empty()) continue;

            mod.raw_name = raw_name;

            // Normalize name: strip priority prefix if configured
            auto prefix_re_str = knowledge.get(game_id, "priority_prefix_re", "");
            if (!prefix_re_str.empty()) {
                try {
                    static const std::regex prefix_re(prefix_re_str);
                    // Extract the numeric prefix value before stripping
                    std::smatch m;
                    if (std::regex_search(raw_name, m, prefix_re)) {
                        auto prefix_str = m.str();
                        // Strip non-digits to get the number
                        std::string digits;
                        for (char c : prefix_str) {
                            if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
                        }
                        if (!digits.empty()) {
                            try { mod.priority = std::stoi(digits); } catch (...) {}
                        }
                    }
                    mod.display_name = std::regex_replace(raw_name, prefix_re, "");
                } catch (...) {
                    mod.display_name = raw_name;
                }
            } else {
                mod.display_name = raw_name;
            }

            // Trim whitespace from display name
            auto first = mod.display_name.find_first_not_of(" \t");
            if (first != std::string::npos) {
                mod.display_name = mod.display_name.substr(first);
            }

            // Parse version
            mod.version = xml_find_tag(content, version_tag);
        } else {
            // MO2-style meta.ini: the folder name IS the mod name; the ini
            // carries the version. A legacy metadata.xml (written by older GMM
            // installs for every game) is read as a fallback so pre-fix mods
            // keep loading - it is never written for non-XML games again.
            auto meta_path = entry.path() / "meta.ini";
            auto content = read_file_text(meta_path);
            if (content.empty()) {
                auto legacy_path = entry.path() / "metadata.xml";
                auto legacy = read_file_text(legacy_path);
                if (legacy.empty()) continue;
                mod.raw_name = folder_name;
                mod.display_name = folder_name;
                mod.version = xml_find_tag(legacy, "version");
            } else {
                engine::ModMeta meta;
                if (!meta.parse(content)) continue;
                mod.raw_name = folder_name;
                mod.display_name = folder_name;
                mod.version = meta.get("General", "version");
            }
        }

        // Check for disable sentinel
        if (!disable_file.empty()) {
            auto disable_path = entry.path() / disable_file;
            mod.enabled = !std::filesystem::exists(disable_path);
        }

        mods.push_back(std::move(mod));
    }

    // Sort: separators first (in insertion order), then by priority if available, else alphabetical
    std::sort(mods.begin(), mods.end(), [](const ScannedMod& a, const ScannedMod& b) {
        if (a.is_separator != b.is_separator) return a.is_separator;
        if (a.priority >= 0 && b.priority >= 0) return a.priority < b.priority;
        if (a.priority >= 0) return true;  // has priority, comes before alphabetical
        if (b.priority >= 0) return false;
        return a.display_name < b.display_name;
    });

    Logger::instance().debug("ModScanner: found " + std::to_string(mods.size()) +
                             " mods in " + mods_dir.string());

    return mods;
}

std::vector<ScannedMod> ModScanner::scan(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& game_install_dir,
    const std::vector<std::filesystem::path>& ignore_symlink_targets) {

    auto mods_subpath = knowledge.get(game_id, "mods_subpath", "");
    std::filesystem::path mods_dir;
    if (!mods_subpath.empty()) {
        mods_dir = game_install_dir / mods_subpath;
    } else {
        mods_dir = game_install_dir;
    }
    return scan_impl(knowledge, game_id, mods_dir, ignore_symlink_targets);
}

std::vector<ScannedMod> ModScanner::scan_dir(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& mods_dir,
    const std::vector<std::filesystem::path>& ignore_symlink_targets) {

    return scan_impl(knowledge, game_id, mods_dir, ignore_symlink_targets);
}

bool ModScanner::disable_mod(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& mod_folder) {
    auto disable_file = knowledge.get(game_id, "disable_mechanism", "");
    if (disable_file.empty()) return false;

    std::ofstream f(mod_folder / disable_file);
    return f.good();
}

bool ModScanner::enable_mod(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& mod_folder) {
    auto disable_file = knowledge.get(game_id, "disable_mechanism", "");
    if (disable_file.empty()) return false;

    std::error_code ec;
    return std::filesystem::remove(mod_folder / disable_file, ec);
}

bool ModScanner::set_priority(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& mod_folder,
    int priority) {
    auto metadata_file = knowledge.get(game_id, "metadata_file", "meta.ini");
    auto name_tag = knowledge.get(game_id, "metadata_name_tag", "name");
    auto prefix_re_str = knowledge.get(game_id, "priority_prefix_re", "");
    auto format_str = knowledge.get(game_id, "priority_format", "%03d ");

    if (metadata_file.empty() || name_tag.empty()) return false;

    auto metadata_path = mod_folder / metadata_file;
    auto content = read_file_text(metadata_path);
    if (content.empty()) return false;

    auto old_name = xml_find_tag(content, name_tag);
    if (old_name.empty()) return false;

    // Strip existing prefix
    std::string clean_name = old_name;
    if (!prefix_re_str.empty()) {
        try {
            static const std::regex prefix_re(prefix_re_str);
            clean_name = std::regex_replace(old_name, prefix_re, "");
        } catch (...) {}
    }

    // Trim whitespace
    auto first = clean_name.find_first_not_of(" \t");
    if (first != std::string::npos) {
        clean_name = clean_name.substr(first);
    }

    // Build new name with prefix
    char buf[64];
    std::snprintf(buf, sizeof(buf), format_str.c_str(), priority);
    std::string new_name = std::string(buf) + clean_name;

    // Replace tag content
    auto open_tag = "<" + name_tag + ">";
    auto close_tag = "</" + name_tag + ">";
    auto open_pos = content.find(open_tag);
    auto close_pos = content.find(close_tag);
    if (open_pos == std::string::npos || close_pos == std::string::npos) return false;

    content.replace(open_pos, close_pos + close_tag.size() - open_pos,
                    open_tag + new_name + close_tag);

    std::ofstream fout(metadata_path);
    if (!fout) return false;
    fout << content;
    return fout.good();
}

}  // namespace engine
