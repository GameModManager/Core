#include "engine/deploy/strategy_overlayfs.h"
#include "engine/log/logger.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace engine {

OverlayFsStrategy::OverlayFsStrategy() = default;

OverlayFsStrategy::~OverlayFsStrategy() {
    if (mounted_) {
        unmount();
    }
}

bool OverlayFsStrategy::deploy(const std::filesystem::path& source,
                                const std::filesystem::path& target) {
    // For OverlayFS, deploy is handled by the mount itself -
    // individual file mappings aren't needed when lowerdir order IS the priority order
    (void)source;
    (void)target;
    return true;
}

bool OverlayFsStrategy::remove(const std::filesystem::path& target) {
    // Same - removal is done by remounting without the mod's lowerdir
    (void)target;
    return true;
}

bool OverlayFsStrategy::mount(const std::filesystem::path& mount_point,
                               const std::vector<std::filesystem::path>& lower_dirs) {
    std::lock_guard lock(mutex_);

    if (mounted_) {
        Logger::instance().warn("OverlayFS already mounted at " + mount_point.string());
        return false;
    }

    if (lower_dirs.empty()) {
        Logger::instance().error("OverlayFS mount: no lower directories provided");
        return false;
    }

    if (!is_available()) {
        Logger::instance().error("OverlayFS not available on this system");
        return false;
    }

    // Create mount point and work dir
    std::error_code ec;
    std::filesystem::create_directories(mount_point, ec);
    if (ec) {
        Logger::instance().error("Failed to create mount point: " + mount_point.string());
        return false;
    }

    work_dir_ = mount_point.parent_path() / (mount_point.filename().string() + "_work");
    std::filesystem::create_directories(work_dir_, ec);

    // Build lowerdir string: first entry = highest priority (first to be looked up)
    std::string lowerdir_str;
    for (size_t i = 0; i < lower_dirs.size(); ++i) {
        if (i > 0) lowerdir_str += ":";
        lowerdir_str += lower_dirs[i].string();
    }

    // Build mount command: mount -t overlay overlay -o lowerdir=X,workdir=Y mount_point
    std::string cmd = "mount -t overlay overlay -o lowerdir=" + lowerdir_str +
                      ",workdir=" + work_dir_.string() + " " + mount_point.string();

    Logger::instance().debug("OverlayFS mount: " + cmd);

    int result = std::system(cmd.c_str());
    if (result != 0) {
        Logger::instance().error("OverlayFS mount failed with exit code " + std::to_string(result));
        return false;
    }

    mount_point_ = mount_point;
    mounted_ = true;

    Logger::instance().debug("OverlayFS mounted at " + mount_point.string() +
        " with " + std::to_string(lower_dirs.size()) + " layers");
    return true;
}

bool OverlayFsStrategy::remount(const std::vector<std::filesystem::path>& lower_dirs) {
    std::lock_guard lock(mutex_);

    if (!mounted_) {
        Logger::instance().error("OverlayFS remount: not mounted");
        return false;
    }

    if (lower_dirs.empty()) {
        Logger::instance().error("OverlayFS remount: no lower directories provided");
        return false;
    }

    std::string lowerdir_str;
    for (size_t i = 0; i < lower_dirs.size(); ++i) {
        if (i > 0) lowerdir_str += ":";
        lowerdir_str += lower_dirs[i].string();
    }

    // Remount with new lowerdir order
    std::string cmd = "mount -t overlay overlay -o remount,lowerdir=" + lowerdir_str +
                      " " + mount_point_.string();

    Logger::instance().debug("OverlayFS remount: " + cmd);

    int result = std::system(cmd.c_str());
    if (result != 0) {
        Logger::instance().error("OverlayFS remount failed with exit code " + std::to_string(result));
        return false;
    }

    Logger::instance().debug("OverlayFS remounted with " + std::to_string(lower_dirs.size()) + " layers");
    return true;
}

bool OverlayFsStrategy::unmount() {
    std::lock_guard lock(mutex_);

    if (!mounted_) return false;

    std::string cmd = "umount " + mount_point_.string();
    int result = std::system(cmd.c_str());

    if (result != 0) {
        Logger::instance().error("OverlayFS unmount failed with exit code " + std::to_string(result));
        return false;
    }

    // Clean up work dir
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);

    mounted_ = false;
    Logger::instance().debug("OverlayFS unmounted from " + mount_point_.string());
    return true;
}

bool OverlayFsStrategy::is_available() {
    // Check if overlayfs module is loaded
    std::ifstream modules("/proc/filesystems");
    if (!modules.is_open()) return false;

    std::string line;
    while (std::getline(modules, line)) {
        if (line.find("overlay") != std::string::npos) return true;
    }

    return false;
}

bool OverlayFsStrategy::can_mount_unprivileged() {
    // Unprivileged overlayfs requires:
    // 1. Kernel >= 5.11 with userxattr support
    // 2. fusermount3 available
    // 3. No upper dir needed (read-only overlay)

    // Check kernel version
    std::ifstream version("/proc/version");
    if (!version.is_open()) return false;

    std::string content;
    std::getline(version, content);

    // Extract major version number - simple check for >= 5.11
    size_t dot_pos = content.find('.');
    if (dot_pos == std::string::npos) return false;

    // Find the major version (number before first dot after space)
    size_t space_pos = content.rfind(' ', dot_pos);
    if (space_pos == std::string::npos) return false;

    int major = 0;
    for (size_t i = space_pos + 1; i < dot_pos; ++i) {
        if (std::isdigit(content[i])) {
            major = major * 10 + (content[i] - '0');
        }
    }

    if (major < 5) return false;
    if (major == 5) {
        // Check minor version
        size_t second_dot = content.find('.', dot_pos + 1);
        if (second_dot == std::string::npos) return false;

        int minor = 0;
        for (size_t i = dot_pos + 1; i < second_dot; ++i) {
            if (std::isdigit(content[i])) {
                minor = minor * 10 + (content[i] - '0');
            }
        }
        if (minor < 11) return false;
    }

    // Check for fusermount3
    int result = std::system("which fusermount3 > /dev/null 2>&1");
    return result == 0;
}

}  // namespace engine
