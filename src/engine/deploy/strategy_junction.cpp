#include "engine/deploy/strategy_junction.h"

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>

#include <filesystem>
#include <vector>

namespace engine {

namespace {

bool create_junction(const std::filesystem::path& target,
                     const std::filesystem::path& source) {
    // Ensure target parent exists
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);

    // Remove existing target
    auto attrs = GetFileAttributesW(target.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveDirectoryW(target.wstring().c_str());
        } else {
            DeleteFileW(target.wstring().c_str());
        }
    }

    // Build NTFS mount point reparse data
    // SubstituteName = \??\<absolute source path>
    // PrintName = <source path>  (empty for junction points)
    std::wstring subst_name = L"\\??\\" + source.wstring();
    std::wstring print_name;

    USHORT subst_bytes = static_cast<USHORT>(subst_name.size() * sizeof(WCHAR));
    USHORT print_bytes = static_cast<USHORT>(print_name.size() * sizeof(WCHAR));

    // PathBuffer layout: [subst_name] [print_name]
    USHORT path_buffer_size = subst_bytes + print_bytes;
    USHORT reparse_data_len = path_buffer_size +
        sizeof(MOUNT_POINT_REPARSE_BUFFER) - sizeof(WCHAR);

    std::vector<BYTE> buffer(sizeof(REPARSE_DATA_BUFFER) + path_buffer_size);
    auto* rdb = reinterpret_cast<REPARSE_DATA_BUFFER*>(buffer.data());

    rdb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rdb->ReparseDataLength = reparse_data_len;
    rdb->Reserved = 0;

    auto& mp = rdb->MountPointReparseBuffer;
    mp.SubstituteNameOffset = 0;
    mp.SubstituteNameLength = subst_bytes;
    mp.PrintNameOffset = subst_bytes;
    mp.PrintNameLength = print_bytes;

    // Copy names into PathBuffer
    std::memcpy(mp.PathBuffer, subst_name.c_str(), subst_bytes);
    std::memcpy(reinterpret_cast<BYTE*>(mp.PathBuffer) + subst_bytes,
                print_name.c_str(), print_bytes);

    HANDLE h = CreateFileW(
        target.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD bytes_returned = 0;
    BOOL ok = DeviceIoControl(
        h, FSCTL_SET_REPARSE_POINT,
        buffer.data(), static_cast<DWORD>(buffer.size()),
        nullptr, 0, &bytes_returned, nullptr);

    CloseHandle(h);
    return ok != FALSE;
}

}  // namespace

bool JunctionStrategy::deploy(const std::filesystem::path& source,
                              const std::filesystem::path& target) {
    std::error_code ec;

    if (std::filesystem::is_directory(source, ec)) {
        return create_junction(target, source);
    }

    // Junctions only work for directories — fall back to copy for files
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::copy_file(source, target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

bool JunctionStrategy::remove(const std::filesystem::path& target) {
    auto attrs = GetFileAttributesW(target.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return RemoveDirectoryW(target.wstring().c_str()) != FALSE;
    }
    return false;
}

bool JunctionStrategy::is_available() {
    return true;
}

}  // namespace engine

#else

namespace engine {

bool JunctionStrategy::deploy(const std::filesystem::path&,
                              const std::filesystem::path&) { return false; }
bool JunctionStrategy::remove(const std::filesystem::path&) { return false; }
bool JunctionStrategy::is_available() { return false; }

}  // namespace engine

#endif
