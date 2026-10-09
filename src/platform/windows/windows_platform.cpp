#ifdef _WIN32

#include "platform/windows/windows_platform.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// Windows headers
#include <shellapi.h>
#include <windows.h>

namespace engine {

// --- Helper: expand environment variables ---

namespace {

  std::wstring expand_env(const wchar_t *pattern) {
    wchar_t buf[MAX_PATH];
    DWORD len = ExpandEnvironmentStringsW(pattern, buf, MAX_PATH);
    if (len == 0 || len > MAX_PATH)
      return {};
    return buf;
  }

  std::filesystem::path env_path(const wchar_t *var) {
    auto *val = _wgetenv(var);
    if (val && val[0] != L'\0')
      return val;
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

// Native Windows game dirs - the prefix is the native OS, so the game's
// Documents and Local AppData are the real user folders.
std::filesystem::path
WindowsPlatform::game_documents_dir(uint32_t /*steam_appid*/) const {
  auto path = env_path(L"USERPROFILE");
  if (path.empty())
    return {};
  return path / L"Documents";
}

std::filesystem::path
WindowsPlatform::game_local_appdata_dir(uint32_t /*steam_appid*/) const {
  auto path = env_path(L"LOCALAPPDATA");
  if (path.empty()) {
    path = expand_env(LR"(%USERPROFILE%\AppData\Local)");
  }
  return path;
}

// Native Windows game dirs - the prefix is the native OS, so the native
// Documents dir is what game_documents_dir() already returns.
std::filesystem::path WindowsPlatform::native_documents_dir() const {
  return game_documents_dir(0);
}

std::filesystem::path WindowsPlatform::steam_userdata_dir() const {
  auto root = find_steam_root();
  if (root.empty())
    return {};
  return root / "userdata";
}

// --- Steam discovery ---

std::filesystem::path WindowsPlatform::find_steam_root() const {
  // 1. Try registry first
  auto reg_path = registry_read_string(LR"(SOFTWARE\Valve\Steam)", L"SteamPath");
  if (!reg_path.empty()) {
    // Registry stores forward slashes; normalize
    std::string s = reg_path.string();
    for (auto &c : s) {
      if (c == '/')
        c = '\\';
    }
    auto root = std::filesystem::path(s);
    auto vdf  = root / "steamapps" / "libraryfolders.vdf";
    if (std::filesystem::exists(vdf))
      return root;
  }

  // 2. Try common Windows install paths
  std::vector<std::filesystem::path> candidates = {
      LR"(C:\Program Files (x86)\Steam)",
      LR"(C:\Program Files\Steam)",
      expand_env(LR"(%PROGRAMFILES(X86)%\Steam)"),
      expand_env(LR"(%PROGRAMFILES%\Steam)"),
  };

  for (const auto &root : candidates) {
    auto vdf = root / "steamapps" / "libraryfolders.vdf";
    if (std::filesystem::exists(vdf))
      return root;
  }

  return {};
}

// --- Registry access ---

std::filesystem::path
WindowsPlatform::registry_read_string(const std::wstring &key_path,
                                      const std::wstring &value_name) {
  HKEY hkey;
  LONG result = RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_READ, &hkey);
  if (result != ERROR_SUCCESS)
    return {};

  wchar_t buf[MAX_PATH];
  DWORD buf_size = sizeof(buf);
  DWORD type     = REG_SZ;

  result = RegQueryValueExW(hkey, value_name.c_str(), nullptr, &type,
                            reinterpret_cast<LPBYTE>(buf), &buf_size);

  RegCloseKey(hkey);

  if (result != ERROR_SUCCESS || type != REG_SZ)
    return {};
  return std::filesystem::path(std::wstring(buf, buf_size / sizeof(wchar_t)));
}

// --- Process launch ---

bool WindowsPlatform::launch_executable(const std::filesystem::path &executable,
                                        const std::vector<std::string> &args) const {
  if (!std::filesystem::exists(executable))
    return false;

  // Build command line
  std::wstring cmd = L"\"" + executable.wstring() + L"\"";
  for (const auto &arg : args) {
    cmd += L" \"" + std::wstring(arg.begin(), arg.end()) + L"\"";
  }

  STARTUPINFOW si        = {};
  si.cb                  = sizeof(si);
  PROCESS_INFORMATION pi = {};

  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                           nullptr, &si, &pi);

  if (ok) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  return ok != FALSE;
}

// --- Privilege check ---

bool WindowsPlatform::is_elevated() const {
  BOOL is_admin = FALSE;
  HANDLE token  = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                            &size)) {
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

// --- Home / temp / thread priority ---

std::filesystem::path WindowsPlatform::home_dir() const {
  wchar_t buf[MAX_PATH];
  DWORD len = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  if (len > 0 && len <= MAX_PATH)
    return std::filesystem::path(std::wstring(buf, len));
  return std::filesystem::temp_directory_path();
}

std::filesystem::path WindowsPlatform::temp_dir() const {
  wchar_t buf[MAX_PATH];
  DWORD len = GetEnvironmentVariableW(L"TEMP", buf, MAX_PATH);
  if (len > 0 && len <= MAX_PATH)
    return std::filesystem::path(std::wstring(buf, len));
  return std::filesystem::temp_directory_path();
}

void WindowsPlatform::set_thread_low_priority() const {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

// --- nxm:// protocol handler registration ---

bool WindowsPlatform::register_nxm_handler(const std::filesystem::path &exe_path) {
  std::wstring exe_w = exe_path.wstring();

  // Register under HKCU\Software\Classes\nxm
  auto set_reg = [](const wchar_t *key, const wchar_t *name, const wchar_t *value) {
    HKEY hkey;
    LONG r =
        RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &hkey, nullptr);
    if (r != ERROR_SUCCESS)
      return false;
    r = RegSetValueExW(hkey, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value),
                       (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);
    return r == ERROR_SUCCESS;
  };

  std::wstring shell_cmd = L"\"" + exe_w + L"\" --handle-nxm \"%1\"";

  bool ok = true;
  ok &= set_reg(LR"(Software\Classes\nxm)", nullptr, L"URL:NXM Protocol");
  ok &= set_reg(LR"(Software\Classes\nxm)", L"URL Protocol", L"");
  ok &= set_reg(LR"(Software\Classes\nxm\shell\open\command)", nullptr,
                shell_cmd.c_str());
  return ok;
}

bool WindowsPlatform::unregister_nxm_handler() {
  // Recursively delete the nxm key
  LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, LR"(Software\Classes\nxm)");
  return r == ERROR_SUCCESS;
}

// --- modl:// protocol handler registration ---

bool WindowsPlatform::register_modl_handler(const std::filesystem::path &exe_path) {
  std::wstring exe_w = exe_path.wstring();

  auto set_reg = [](const wchar_t *key, const wchar_t *name, const wchar_t *value) {
    HKEY hkey;
    LONG r =
        RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &hkey, nullptr);
    if (r != ERROR_SUCCESS)
      return false;
    r = RegSetValueExW(hkey, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value),
                       (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);
    return r == ERROR_SUCCESS;
  };

  std::wstring shell_cmd = L"\"" + exe_w + L"\" --handle-modl \"%1\"";

  bool ok = true;
  ok &= set_reg(LR"(Software\Classes\modl)", nullptr, L"URL:MODL Protocol");
  ok &= set_reg(LR"(Software\Classes\modl)", L"URL Protocol", L"");
  ok &= set_reg(LR"(Software\Classes\modl\shell\open\command)", nullptr,
                shell_cmd.c_str());
  return ok;
}

bool WindowsPlatform::unregister_modl_handler() {
  LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, LR"(Software\Classes\modl)");
  return r == ERROR_SUCCESS;
}

bool WindowsPlatform::is_modl_handler_registered() {
  // Check that the protocol key exists and points at a shell\open\command
  // value. The Windows side does not know which executable we expect here
  // (the linux implementation cross-checks the desktop file id) - it just
  // reports whether a modl:// handler has been registered under HKCU at
  // all. The UI treats this as "registered" and shows the right state.
  HKEY hkey;
  LONG r =
      RegOpenKeyExW(HKEY_CURRENT_USER, LR"(Software\Classes\modl\shell\open\command)",
                    0, KEY_READ, &hkey);
  if (r != ERROR_SUCCESS)
    return false;
  wchar_t buf[1024] = {};
  DWORD buf_size    = sizeof(buf);
  DWORD type        = REG_SZ;
  r = RegQueryValueExW(hkey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                       &buf_size);
  RegCloseKey(hkey);
  if (r != ERROR_SUCCESS || type != REG_SZ || buf_size == 0)
    return false;
  // The command must reference our flag. The exact binary path is not
  // portable across installations, so a substring check on "--handle-modl"
  // is good enough.
  std::wstring cmd(buf, (buf_size / sizeof(wchar_t)) - 1);
  return cmd.find(L"--handle-modl") != std::wstring::npos;
}

// --- Platform protocol handler virtuals ---
//
// Windows has one registry shape for every scheme: a per-user
// HKCU\Software\Classes\<scheme> key carrying "URL Protocol" plus a
// shell\open\command value. The per-protocol registrars above predate this
// table and stay for their existing callers; everything else goes through here.

namespace {

  struct SchemeSpec {
    const wchar_t *scheme;
    const wchar_t *flag;  // argv flag only our own registration writes
    const wchar_t *desc;  // default value of the scheme key
  };

  SchemeSpec spec_for(ProtocolHandler protocol) {
    switch (protocol) {
    case ProtocolHandler::Nxm:
      return {L"nxm", L"--handle-nxm", L"URL:NXM Protocol"};
    case ProtocolHandler::Gmm:
      return {L"gmm", L"--handle-gmm", L"URL:GMM Protocol"};
    case ProtocolHandler::Modl:
      return {L"modl", L"--handle-modl", L"URL:MODL Protocol"};
    }
    return {nullptr, nullptr, nullptr};
  }

  // "Software\Classes\nxm" plus an optional sub-path tail.
  std::wstring class_key(ProtocolHandler protocol, const wchar_t *tail) {
    const SchemeSpec spec = spec_for(protocol);
    if (!spec.scheme)
      return {};
    std::wstring key = LR"(Software\Classes\)" + std::wstring(spec.scheme);
    if (tail)
      key += tail;
    return key;
  }

  bool write_reg(const std::wstring &key_path, const wchar_t *name,
                 const wchar_t *value) {
    HKEY hkey;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hkey,
                             nullptr);
    if (r != ERROR_SUCCESS)
      return false;
    r = RegSetValueExW(hkey, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value),
                       (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);
    return r == ERROR_SUCCESS;
  }

  bool read_reg_string(const std::wstring &key_path, const wchar_t *name,
                       std::wstring &out) {
    HKEY hkey;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, KEY_READ, &hkey);
    if (r != ERROR_SUCCESS)
      return false;
    wchar_t buf[2048] = {};
    DWORD buf_size    = sizeof(buf);
    DWORD type        = REG_SZ;
    r = RegQueryValueExW(hkey, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                         &buf_size);
    RegCloseKey(hkey);
    if (r != ERROR_SUCCESS || type != REG_SZ || buf_size == 0)
      return false;
    out.assign(buf, (buf_size / sizeof(wchar_t)) - 1);
    return true;
  }

  std::string narrow(const std::wstring &s) {
    if (s.empty())
      return {};
    const int need =
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr,
                            0, nullptr, nullptr);
    if (need <= 0)
      return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(),
                        need, nullptr, nullptr);
    return out;
  }

}  // namespace

bool WindowsPlatform::register_protocol_handler(
    ProtocolHandler protocol, const std::filesystem::path &exe_path) const {
  const SchemeSpec spec = spec_for(protocol);
  if (!spec.scheme)
    return false;

  const std::wstring root    = class_key(protocol, nullptr);
  const std::wstring cmd_key = class_key(protocol, LR"(\shell\open\command)");
  const std::wstring shell_cmd =
      L"\"" + exe_path.wstring() + L"\" " + spec.flag + L" \"%1\"";

  bool ok = true;
  ok &= write_reg(root, nullptr, spec.desc);
  ok &= write_reg(root, L"URL Protocol", L"");
  ok &= write_reg(cmd_key, nullptr, shell_cmd.c_str());
  return ok;
}

bool WindowsPlatform::unregister_protocol_handler(ProtocolHandler protocol) const {
  const std::wstring root = class_key(protocol, nullptr);
  if (root.empty())
    return false;
  return RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()) == ERROR_SUCCESS;
}

bool WindowsPlatform::is_protocol_handler_registered(ProtocolHandler protocol) const {
  std::wstring cmd;
  if (!read_reg_string(class_key(protocol, LR"(\shell\open\command)"), nullptr, cmd))
    return false;
  // The binary path differs per installation, so match the argv flag instead:
  // it is what our own registration writes and nothing else does.
  const SchemeSpec spec = spec_for(protocol);
  return cmd.find(spec.flag) != std::wstring::npos;
}

std::string WindowsPlatform::current_protocol_handler(ProtocolHandler protocol) const {
  std::wstring cmd;
  if (!read_reg_string(class_key(protocol, LR"(\shell\open\command)"), nullptr, cmd))
    return {};
  // `"C:\...\GameModManager.exe" --handle-nxm "%1"` -> "GameModManager.exe".
  const std::size_t quoted = cmd.find(L'"');
  std::wstring exe         = quoted == std::wstring::npos ? cmd : cmd.substr(0, quoted);
  const std::size_t sep    = exe.find_last_of(L"\\/");
  if (sep != std::wstring::npos)
    exe = exe.substr(sep + 1);
  return narrow(exe);
}

// --- Free-function forms (see platform.h) ---

std::string platform_id() { return WindowsPlatform().platform_name(); }

std::filesystem::path find_steam_root() { return WindowsPlatform().find_steam_root(); }

std::filesystem::path default_cache_dir() { return WindowsPlatform().cache_dir(); }

std::filesystem::path find_wine() { return WindowsPlatform().find_wine(); }

bool register_protocol_handler(ProtocolHandler protocol,
                               const std::filesystem::path &exe_path) {
  return WindowsPlatform().register_protocol_handler(protocol, exe_path);
}

bool unregister_protocol_handler(ProtocolHandler protocol) {
  return WindowsPlatform().unregister_protocol_handler(protocol);
}

bool is_protocol_handler_registered(ProtocolHandler protocol) {
  return WindowsPlatform().is_protocol_handler_registered(protocol);
}

std::string current_protocol_handler(ProtocolHandler protocol) {
  return WindowsPlatform().current_protocol_handler(protocol);
}

}  // namespace engine

#endif  // _WIN32
