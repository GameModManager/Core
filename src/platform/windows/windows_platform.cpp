#ifdef _WIN32

#include "platform/windows/windows_platform.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// Windows headers
#include <windows.h>
#include <shellapi.h>

namespace engine {

// --- Helper: expand environment variables ---

namespace {

std::wstring expand_env(const wchar_t* pattern) {
    wchar_t buf[MAX_PATH];
    DWORD len = ExpandEnvironmentStringsW(pattern, buf, MAX_PATH);
    if (len == 0 || len > MAX_PATH) return {};
    return buf;
}

std::filesystem::path env_path(const wchar_t* var) {
    auto* val = _wgetenv(var);
    if (val && val[0] != L'\0') return val;
    return {};
}

}  // namespace

// --- Directory resolution ---

std::filesystem::path WindowsPlatform::appdata_dir() const {
    auto path = env_path(L"APPDATA");
    if (path.empty()) {
        path = expand_env(LR"(%USERPROFILE%\AppData\Roaming)");
    }
    return path / L"gamemodmanager";
}

std::filesystem::path WindowsPlatform::localappdata_dir() const {
    auto path = env_path(L"LOCALAPPDATA");
    if (path.empty()) {
        path = expand_env(LR"(%USERPROFILE%\AppData\Local)");
    }
    return path / L"gamemodmanager";
}

std::filesystem::path WindowsPlatform::data_dir() const {
    return localappdata_dir();
}

std::filesystem::path WindowsPlatform::config_dir() const {
    return appdata_dir();
}

std::filesystem::path WindowsPlatform::cache_dir() const {
    return localappdata_dir() / L"cache";
}

// --- Steam discovery ---

std::filesystem::path WindowsPlatform::find_steam_root() const {
    // 1. Try registry first
    auto reg_path = registry_read_string(
        LR"(SOFTWARE\Valve\Steam)", L"SteamPath");
    if (!reg_path.empty()) {
        // Registry stores forward slashes; normalize
        std::string s = reg_path.string();
        for (auto& c : s) {
            if (c == '/') c = '\\';
        }
        auto root = std::filesystem::path(s);
        auto vdf = root / "steamapps" / "libraryfolders.vdf";
        if (std::filesystem::exists(vdf)) return root;
    }

    // 2. Try common Windows install paths
    std::vector<std::filesystem::path> candidates = {
        LR"(C:\Program Files (x86)\Steam)",
        LR"(C:\Program Files\Steam)",
        expand_env(LR"(%PROGRAMFILES(X86)%\Steam)"),
        expand_env(LR"(%PROGRAMFILES%\Steam)"),
    };

    for (const auto& root : candidates) {
        auto vdf = root / "steamapps" / "libraryfolders.vdf";
        if (std::filesystem::exists(vdf)) return root;
    }

    return {};
}

// --- Registry access ---

std::filesystem::path WindowsPlatform::registry_read_string(
    const std::wstring& key_path, const std::wstring& value_name) {
    HKEY hkey;
    LONG result = RegOpenKeyExW(
        HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_READ, &hkey);
    if (result != ERROR_SUCCESS) return {};

    wchar_t buf[MAX_PATH];
    DWORD buf_size = sizeof(buf);
    DWORD type = REG_SZ;

    result = RegQueryValueExW(
        hkey, value_name.c_str(), nullptr, &type,
        reinterpret_cast<LPBYTE>(buf), &buf_size);

    RegCloseKey(hkey);

    if (result != ERROR_SUCCESS || type != REG_SZ) return {};
    return std::filesystem::path(std::wstring(buf, buf_size / sizeof(wchar_t)));
}

// --- Process launch ---

bool WindowsPlatform::launch_executable(
    const std::filesystem::path& executable,
    const std::vector<std::string>& args) const {
    if (!std::filesystem::exists(executable)) return false;

    // Build command line
    std::wstring cmd = L"\"" + executable.wstring() + L"\"";
    for (const auto& arg : args) {
        cmd += L" \"" + std::wstring(arg.begin(), arg.end()) + L"\"";
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        nullptr, cmd.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &si, &pi);

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
}

// --- Privilege check ---

bool WindowsPlatform::is_elevated() const {
    BOOL is_admin = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &size)) {
            is_admin = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return is_admin != FALSE;
}

// --- Symlink availability ---

bool WindowsPlatform::symlinks_available() const {
    // Symlinks on Windows require either:
    // - SeCreateSymbolicLinkPrivilege (admin or Developer Mode enabled)
    // - The process is elevated
    return is_elevated();
}

// --- nxm:// protocol handler registration ---

bool WindowsPlatform::register_nxm_handler(
    const std::filesystem::path& exe_path) {
    std::wstring exe_w = exe_path.wstring();

    // Register under HKCU\Software\Classes\nxm
    auto set_reg = [](const wchar_t* key, const wchar_t* name,
                      const wchar_t* value) {
        HKEY hkey;
        LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                 nullptr, &hkey, nullptr);
        if (r != ERROR_SUCCESS) return false;
        r = RegSetValueExW(hkey, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(value),
                           (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(hkey);
        return r == ERROR_SUCCESS;
    };

    std::wstring shell_cmd = L"\"" + exe_w + L"\" --handle-nxm \"%1\"";

    bool ok = true;
    ok &= set_reg(LR"(Software\Classes\nxm)", nullptr, L"URL:NXM Protocol");
    ok &= set_reg(LR"(Software\Classes\nxm)", L"URL Protocol", L"");
    ok &= set_reg(LR"(Software\Classes\nxm\shell\open\command)",
                  nullptr, shell_cmd.c_str());
    return ok;
}

bool WindowsPlatform::unregister_nxm_handler() {
    // Recursively delete the nxm key
    LONG r = RegDeleteTreeW(HKEY_CURRENT_USER,
                            LR"(Software\Classes\nxm)");
    return r == ERROR_SUCCESS;
}

}  // namespace engine

#endif  // _WIN32
