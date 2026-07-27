#include "engine/detect/mod_scanner.h"
#include "engine/registry/game_knowledge.h"
#include "engine/log/logger.h"

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

std::vector<ScannedMod> ModScanner::scan(
    const GameKnowledge& knowledge,
    const std::string& game_id,
    const std::filesystem::path& game_install_dir) {

    // Read all config from GameKnowledge hooks
    auto mods_subpath = knowledge.get(game_id, "mods_subpath", "");
    auto disable_file = knowledge.get(game_id, "disable_mechanism", "");
    auto ignored_csv  = knowledge.get(game_id, "ignored_files", "");
    auto metadata_file = knowledge.get(game_id, "metadata_file", "metadata.xml");
    auto name_tag     = knowledge.get(game_id, "metadata_name_tag", "name");
    auto version_tag  = knowledge.get(game_id, "metadata_version_tag", "version");
    auto separator_suffix = knowledge.get(game_id, "separator_suffix", "_separator");

    auto ignored = split_csv(ignored_csv);
    // Always ignore the metadata file and disable sentinel during directory scanning
    if (!metadata_file.empty() && should_ignore(metadata_file, ignored) == false) {
        ignored.push_back(metadata_file);
    }
    if (!disable_file.empty() && should_ignore(disable_file, ignored) == false) {
        ignored.push_back(disable_file);
    }

    // Resolve mods directory
    std::filesystem::path mods_dir;
    if (!mods_subpath.empty()) {
        mods_dir = game_install_dir / mods_subpath;
    } else {
        mods_dir = game_install_dir;
    }

    std::vector<ScannedMod> mods;
    std::error_code ec;

    if (!std::filesystem::exists(mods_dir)) {
        Logger::instance().warn("ModScanner: mods directory not found: " + mods_dir.string());
        return mods;
    }

    for (const auto& entry : std::filesystem::directory_iterator(mods_dir)) {
        if (!entry.is_directory()) continue;

        auto folder_name = entry.path().filename().string();
        if (should_ignore(folder_name, ignored)) continue;

        ScannedMod mod;
        mod.folder_name = folder_name;

        // Check for separator (game-specific suffix)
        if (!separator_suffix.empty() &&
            folder_name.size() > separator_suffix.size() &&
            folder_name.compare(folder_name.size() - separator_suffix.size(),
                                separator_suffix.size(),
                                separator_suffix) == 0) {
            mod.is_separator = true;

            // Parse separator.xml for name + color
            auto sep_xml = entry.path() / "separator.xml";
            auto sep_content = read_file_text(sep_xml);
            if (!sep_content.empty()) {
                auto name = xml_find_tag(sep_content, "name");
                auto color = xml_find_tag(sep_content, "color");
                mod.display_name = name.empty()
                    ? folder_name.substr(0, folder_name.size() - separator_suffix.size())
                    : name;
                mod.separator_color = color.empty() ? "#888888" : color;
            } else {
                mod.display_name = folder_name.substr(0, folder_name.size() - separator_suffix.size());
                mod.separator_color = "#888888";
            }
            mod.raw_name = folder_name;
            mods.push_back(std::move(mod));
            continue;
        }

        // Parse metadata file
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

    Logger::instance().info("ModScanner: found " + std::to_string(mods.size()) +
                            " mods in " + mods_dir.string());

    return mods;
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
    auto metadata_file = knowledge.get(game_id, "metadata_file", "metadata.xml");
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

bool ModScanner::symlink_overwrite(const std::filesystem::path& game_mods_dir,
                                    const std::filesystem::path& overwrite_dir) {
    if (game_mods_dir.empty() || overwrite_dir.empty()) return false;
    if (!std::filesystem::exists(game_mods_dir)) return false;

    auto link_path = game_mods_dir / "Overwrite";
    std::error_code ec;

    if (std::filesystem::exists(link_path, ec)) {
        if (std::filesystem::is_symlink(link_path, ec)) {
            auto target = std::filesystem::read_symlink(link_path, ec);
            if (target == overwrite_dir) return true;
            std::filesystem::remove(link_path, ec);
        } else {
            return false;
        }
    }

    std::filesystem::create_symlink(overwrite_dir, link_path, ec);
    if (ec) {
        Logger::instance().error("Failed to create Overwrite symlink: " + ec.message());
        return false;
    }

    Logger::instance().info("Overwrite symlinked: " + link_path.string() +
                            " -> " + overwrite_dir.string());
    return true;
}

}  // namespace engine
