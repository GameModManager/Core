#include "engine/deploy/overwrite_capture.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace engine {

namespace {

// Default patterns to ignore - game engine caches, saves, etc.
const std::vector<std::string> kDefaultIgnored = {
    ".git", ".svn",
    "__pycache__",
    "*.tmp", "*.temp",
    "*.bak",
    "*.log",  // capture logs separately if needed
};

bool matches_pattern(const std::string& filename, const std::string& pattern) {
    // Simple glob: exact match or prefix/suffix with *
    if (pattern.empty()) return false;
    if (pattern.front() == '*') {
        auto suffix = pattern.substr(1);
        return filename.size() >= suffix.size() &&
               filename.compare(filename.size() - suffix.size(),
                                suffix.size(), suffix) == 0;
    }
    if (pattern.back() == '*') {
        auto prefix = pattern.substr(0, pattern.size() - 1);
        return filename.compare(0, prefix.size(), prefix) == 0;
    }
    return filename == pattern;
}

}  // namespace

bool OverwriteCapture::should_ignore(const std::string& relative_path,
                                     const std::vector<std::string>& patterns) {
    auto filename = std::filesystem::path(relative_path).filename().string();
    for (const auto& pat : patterns) {
        if (matches_pattern(filename, pat)) return true;
    }
    return false;
}

std::vector<std::string> OverwriteCapture::capture(const Config& config) {
    std::vector<std::string> captured;
    std::error_code ec;

    if (!std::filesystem::exists(config.game_dir)) return captured;
    if (!std::filesystem::exists(config.overwrite_dir)) {
        std::filesystem::create_directories(config.overwrite_dir, ec);
    }

    // Build a set of deployed paths for O(1) lookup
    std::unordered_set<std::string> deployed(
        config.deployed_relative_paths.begin(),
        config.deployed_relative_paths.end());

    // Merge default ignored patterns with config
    auto ignored = config.ignored_patterns;
    ignored.insert(ignored.end(), kDefaultIgnored.begin(), kDefaultIgnored.end());

    // Walk the game directory
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(config.game_dir)) {
        if (!entry.is_regular_file()) continue;

        auto rel = std::filesystem::relative(entry.path(), config.game_dir, ec);
        if (ec) continue;

        auto rel_str = rel.string();

        // Skip files belonging to deployed mods
        if (deployed.count(rel_str)) continue;

        // Skip ignored patterns
        if (should_ignore(rel_str, ignored)) continue;

        // Copy to Overwrite
        auto dest = config.overwrite_dir / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(entry.path(), dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            captured.push_back(rel_str);
        }
    }

    return captured;
}

bool OverwriteCapture::clear(const std::filesystem::path& overwrite_dir) {
    std::error_code ec;
    // Remove everything except the directory itself
    for (const auto& entry : std::filesystem::directory_iterator(overwrite_dir)) {
        std::filesystem::remove_all(entry.path(), ec);
        if (ec) return false;
    }
    return true;
}

bool OverwriteCapture::promote_to_mod(
    const std::filesystem::path& overwrite_dir,
    const std::filesystem::path& mod_dir,
    const std::vector<std::string>& relative_paths) {
    std::error_code ec;

    for (const auto& rel : relative_paths) {
        auto src = overwrite_dir / rel;
        auto dst = mod_dir / rel;

        if (!std::filesystem::exists(src)) continue;

        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(src, dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;

        // Remove from Overwrite after successful copy
        std::filesystem::remove(src, ec);
    }

    return true;
}

}  // namespace engine
