#include "engine/fs_utils.h"
#include "engine/log/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace engine {

namespace {

// Percent-encode a filesystem path for a freedesktop .trashinfo file.
std::string url_encode_path(const std::filesystem::path& path) {
    const std::string in = path.string();
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '-' ||
            c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string trash_root() {
    const char* data_home = std::getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        return std::string(data_home) + "/Trash";
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::string(home) + "/.local/share/Trash";
}

// freedesktop.org Trash spec (Linux).
bool move_to_trash_linux(const std::filesystem::path& path) {
    const auto trash = trash_root();
    if (trash.empty()) return false;

    const auto files_dir = std::filesystem::path(trash) / "files";
    const auto info_dir = std::filesystem::path(trash) / "info";

    std::error_code ec;
    std::filesystem::create_directories(files_dir, ec);
    std::filesystem::create_directories(info_dir, ec);
    if (ec) return false;

    // Collision-free name: name, name.1, name.2, ...
    const auto base = path.filename();
    auto target = files_dir / base;
    int n = 0;
    while (true) {
        ec.clear();
        if (!std::filesystem::exists(target, ec)) break;
        target = files_dir / (base.string() + "." + std::to_string(++n));
    }

    std::error_code move_ec;
    std::filesystem::rename(path, target, move_ec);
    if (move_ec) {
        // Cross-device fallback: copy recursively, then remove the source.
        std::error_code copy_ec;
        std::filesystem::copy(path, target,
            std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::copy_symlinks, copy_ec);
        if (copy_ec) return false;
        std::filesystem::remove_all(path, ec);
    }

    // Write the .trashinfo sidecar so the entry can be restored.
    char date_buf[32];
    char offset_buf[16];
    const auto now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::strftime(offset_buf, sizeof(offset_buf), "%z", &tm);  // +HHMM / -HHMM
    std::string offset(offset_buf);
    if (offset.size() == 5 && (offset[0] == '+' || offset[0] == '-'))
        offset = offset.substr(0, 3) + ":" + offset.substr(3);

    const auto info_path = info_dir / (target.filename().string() + ".trashinfo");
    std::ofstream info(info_path);
    if (!info) return false;
    info << "[Trash Info]\n"
         << "Path=" << url_encode_path(std::filesystem::absolute(path)) << "\n"
         << "DeletionDate=" << date_buf << offset << "\n";
    return true;
}

}  // namespace

bool remove_path(const std::filesystem::path& path, bool permanent) {
    if (path.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;  // already gone

    if (permanent) {
        std::filesystem::remove_all(path, ec);
        if (ec) {
            Logger::instance().error("Failed to permanently remove " +
                path.string() + ": " + ec.message());
            return false;
        }
        return true;
    }

#if defined(_WIN32)
    const std::wstring w = path.wstring();
    std::vector<wchar_t> from(w.begin(), w.end());
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.data();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&op) != 0) {
        Logger::instance().error("Failed to move " + path.string() +
            " to the Recycle Bin");
        return false;
    }
    return true;
#else
    return move_to_trash_linux(path);
#endif
}

}  // namespace engine
