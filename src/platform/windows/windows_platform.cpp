#ifdef _WIN32

#include "platform/windows/windows_platform.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

// Windows headers
#include <cerrno>
#include <fcntl.h>
#include <io.h>
#include <optional>
#include <process.h>
#include <shellapi.h>
#include <sys/stat.h>
#include <windows.h>

namespace engine {

// --- Helper: expand environment variables ---

namespace {

  // Quote one argument per the CommandLineToArgvW rules, which is what
  // CreateProcessW parses the command line with. A backslash is only special
  // immediately before a quote or at the end of the argument, so the general
  // case is "wrap in quotes and double any interior quotes"; a run of
  // backslashes before a quote or before the closing quote has to be doubled as
  // well or the quote comes out escaped instead of literal.
  std::wstring QuoteForWindows(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
      return arg;

    std::wstring out;
    out.push_back(L'"');
    for (std::size_t i = 0;; ++i) {
      std::size_t backslashes = 0;
      while (i < arg.size() && arg[i] == L'\\') {
        ++i;
        ++backslashes;
      }
      if (i == arg.size()) {
        // Trailing run: double it so the closing quote is not escaped.
        out.append(backslashes * 2, L'\\');
        break;
      }
      if (arg[i] == L'"') {
        out.append(backslashes * 2 + 1, L'\\');
      } else {
        out.append(backslashes, L'\\');
      }
      out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
  }

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

// --- OS primitives that have no Platform instance to hang off ---

std::tm local_time(std::time_t t) {
  std::tm out{};
  localtime_s(&out, &t);
  return out;
}

std::tm utc_time(std::time_t t) {
  std::tm out{};
  gmtime_s(&out, &t);
  return out;
}

long current_process_id() {
  return static_cast<long>(_getpid());
}

void set_thread_low_priority() {
  WindowsPlatform().set_thread_low_priority();
}

bool atomic_replace(const std::filesystem::path &from,
                    const std::filesystem::path &to) {
  // MOVEFILE_WRITE_THROUGH is what makes this a data-integrity guarantee
  // rather than a rename: it does not return until the rename has been
  // flushed to disk, so a power loss immediately afterwards cannot leave the
  // old contents sitting under the new name.
  return MoveFileExW(from.c_str(), to.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool create_truncated_file(const std::filesystem::path &path, std::error_code &ec) {
  ec.clear();
  const std::wstring w = path.wstring();
  const int flags      = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
  const int fd         = ::_wopen(w.c_str(), flags, _S_IREAD | _S_IWRITE);
  if (fd < 0) {
    ec.assign(errno, std::generic_category());
    return false;
  }
  ::_close(fd);
  return true;
}

bool move_to_recycle_bin(const std::filesystem::path &path) {
  // SHFileOperationW takes a double-NUL-terminated multi-string of paths;
  // FOF_ALLOWUNDO is what routes it to the Recycle Bin instead of deleting.
  const std::wstring w = path.wstring();
  std::vector<wchar_t> from(w.begin(), w.end());
  from.push_back(L'\0');
  from.push_back(L'\0');

  SHFILEOPSTRUCTW op{};
  op.wFunc  = FO_DELETE;
  op.pFrom  = from.data();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  return SHFileOperationW(&op) == 0;
}

bool path_is_executable(const std::filesystem::path &path) {
  // NTFS has no execute bit: the ACL is what says whether this user may run
  // the file. _waccess with mode 0 asks "can this user open it at all", which
  // is the closest the CRT gets, and is the same question a PATHEXT lookup
  // ends up asking.
  return ::_waccess(path.wstring().c_str(), 0) == 0;
}

bool filesystem_is_case_insensitive() {
  return true;  // NTFS (and every other volume Windows mounts) folds case
}

std::string machine_id() {
  HKEY hkey = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                    KEY_READ, &hkey) != ERROR_SUCCESS)
    return {};
  wchar_t buf[256];
  DWORD buf_size = sizeof(buf);
  DWORD type     = 0;
  LPBYTE bytes   = reinterpret_cast<LPBYTE>(buf);
  const LONG rc =
      RegQueryValueExW(hkey, L"MachineGuid", nullptr, &type, bytes, &buf_size);
  const bool ok = rc == ERROR_SUCCESS && type == REG_SZ;
  RegCloseKey(hkey);
  if (!ok)
    return {};

  // The GUID is ASCII hex with braces; narrow it byte-per-wchar and drop the
  // trailing NUL that buf_size counts.
  const std::wstring guid(buf, buf_size / sizeof(wchar_t));
  std::string narrow;
  narrow.reserve(guid.size());
  for (wchar_t wc : guid) {
    if (wc != L'\0')
      narrow += static_cast<char>(wc);
  }
  return narrow;
}

std::optional<std::int64_t> file_birth_time(const std::filesystem::path &path) {
  // GetFileTimeEx needs a handle, and GetFileAttributesEx does not expose a
  // creation time, so this is the only route to one on Windows.
  HANDLE h = CreateFileW(path.wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return std::nullopt;
  FILETIME created{};
  const bool ok = GetFileTime(h, &created, nullptr, nullptr) != FALSE;
  CloseHandle(h);
  if (!ok)
    return std::nullopt;

  // FILETIME counts 100ns ticks since 1601-01-01; the Unix epoch is 11644473600
  // seconds later.
  ULARGE_INTEGER ticks{};
  ticks.LowPart  = created.dwLowDateTime;
  ticks.HighPart = created.dwHighDateTime;
  return static_cast<std::int64_t>(ticks.QuadPart / 10000000ULL - 11644473600LL);
}

std::filesystem::path current_executable_path() {
  // MAX_PATH is 260 but a per-user install under a long profile can exceed it,
  // so grow the buffer until the value fits. GetModuleFileNameW returns the
  // untruncated length when the buffer is too small, which is the loop condition.
  std::wstring buf(512, L'\0');
  for (;;) {
    const DWORD n =
        GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0)
      return {};
    if (n < buf.size()) {
      buf.resize(n);
      return std::filesystem::path(buf);
    }
    buf.resize(buf.size() * 2);
  }
}

std::filesystem::path home_dir_or_empty() {
  // USERPROFILE is what Windows sets for an interactive user; HOME is the
  // fallback some tooling (git bash, MSYS) sets instead. Empty when neither is
  // there, so a caller can tell "no home configured" from "home is temp".
  const char *profile = std::getenv("USERPROFILE");
  if (profile && profile[0] != '\0')
    return std::filesystem::path(profile);
  const char *home = std::getenv("HOME");
  return (home && home[0] != '\0') ? std::filesystem::path(home)
                                   : std::filesystem::path{};
}

bool path_is_writable(const std::filesystem::path &dir) {
  if (dir.empty())
    return false;
  // POSIX access() semantics and W_OK do not exist here, and W_OK is undefined
  // for directories. The directory's existence and attribute read is the honest
  // equivalent query, and it is still a pure probe: nothing is created.
  const DWORD attrs = GetFileAttributesW(dir.wstring().c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    return false;
  return true;
}

std::filesystem::path volume_root_of(const std::filesystem::path &p) {
  if (p.empty())
    return {};
  // GetVolumePathNameW walks up to the volume root itself, so no manual
  // ancestor loop is needed.
  std::wstring buf(MAX_PATH, L'\0');
  const DWORD n = GetVolumePathNameW(p.wstring().c_str(), buf.data(),
                                     static_cast<DWORD>(buf.size()));
  if (n == 0 || n >= buf.size())
    return {};
  buf.resize(n);
  return std::filesystem::path(buf);
}

std::filesystem::path recorded_install_location() {
  // The uninstall subkey our installer writes and the detector reads. Kept as
  // one constant on both sides of the contract: an installer that writes a
  // different subkey reports as "not an installer install", which is a visible
  // report rather than a silent wrong answer.
  static constexpr wchar_t kUninstallSubkey[] =
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GameModManager";
  static constexpr wchar_t kInstallLocationValue[] = L"InstallLocation";

  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallSubkey, 0, KEY_READ, &key) !=
      ERROR_SUCCESS)
    return {};

  wchar_t buf[1024] = {};
  DWORD type        = 0;
  DWORD size        = sizeof(buf) - 1;
  const bool ok =
      RegQueryValueExW(key, kInstallLocationValue, nullptr, &type,
                       reinterpret_cast<BYTE *>(buf), &size) == ERROR_SUCCESS;
  RegCloseKey(key);
  if (!ok || (type != REG_SZ && type != REG_EXPAND_SZ) || buf[0] == L'\0')
    return {};
  return std::filesystem::path(buf);
}

void *load_shared_library(const std::filesystem::path &path) {
  // LOAD_WITH_ALTERED_SEARCH_PATH keeps a plugin's own directory ahead of the
  // process-wide search path, so a plugin that ships a dependency beside itself
  // finds that copy rather than whatever else on PATH happens to share the
  // name. Wide entry point because that is what resolves a Unicode path.
  return static_cast<void *>(
      LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

void *shared_library_symbol(void *handle, const char *name) {
  return handle ? reinterpret_cast<void *>(
                      GetProcAddress(static_cast<HMODULE>(handle), name))
                : nullptr;
}

void unload_shared_library(void *handle) noexcept {
  if (handle)
    FreeLibrary(static_cast<HMODULE>(handle));
}

std::int64_t spawn_detached_process(const std::filesystem::path &executable,
                                    const std::vector<std::string> &argv,
                                    const std::filesystem::path &work_dir,
                                    bool /*shell_fallback*/) {
  if (argv.empty() || executable.empty())
    return -1;

  // argv[0] is passed through verbatim and is not necessarily the executable,
  // so the command line starts from argv rather than from `executable`.
  std::wstring cmd = QuoteForWindows(std::wstring(argv[0].begin(), argv[0].end()));
  for (std::size_t i = 1; i < argv.size(); ++i) {
    cmd.push_back(L' ');
    cmd += QuoteForWindows(std::wstring(argv[i].begin(), argv[i].end()));
  }
  const std::wstring exe(executable.wstring());

  // STARTUPINFO must be zeroed or CreateProcess reads whatever is on the stack.
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  const std::wstring wdir(work_dir.wstring());
  const wchar_t *cwd = wdir.empty() ? nullptr : wdir.c_str();

  // DETACHED_PROCESS so the game outlives us and does not inherit our console;
  // CREATE_NEW_PROCESS_GROUP so Ctrl-C in our terminal does not reach it. The
  // command line is mutable, so cmd.data() is handed over as-is.
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, cwd, &si,
                      &pi))
    return -1;

  CloseHandle(pi.hThread);
  const auto pid = static_cast<std::int64_t>(pi.dwProcessId);
  // Closed rather than kept: nothing here waits on the child, and holding it
  // would leak one handle per launch.
  CloseHandle(pi.hProcess);
  return pid;
}

const char *dlerror_message() {
  // Windows has no dlerror. GetLastError is the equivalent, but it is a DWORD
  // and the only way to render it without a static buffer is FormatMessage into
  // one. A thread_local buffer keeps this correct when two threads fail a load
  // at once, which is exactly what the plugin scanner does during a scan.
  static thread_local char buf[256] = {};
  const DWORD err                   = GetLastError();
  if (err == 0)
    return "";
  DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
                           sizeof(buf) - 1, nullptr);
  if (n == 0) {
    std::snprintf(buf, sizeof(buf), "LoadLibrary failed (error %lu)",
                  static_cast<unsigned long>(err));
    return buf;
  }
  // FormatMessage appends CRLF; a log line does not want it.
  while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'))
    buf[--n] = '\0';
  return buf;
}

int open_truncated_write_fd(const std::string &path) {
  // _open on the narrow path: the logger is handed an ASCII-ish path by
  // main() and taking the wide form here would mean threading std::wstring
  // through the Logger for no gain at this layer.
  return ::_open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                 _S_IREAD | _S_IWRITE);
}

void write_raw_fd(int fd, const char *data, std::size_t size) {
  while (size > 0) {
    const int n = ::_write(fd, data, static_cast<unsigned>(size));
    if (n <= 0)
      return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}

void close_raw_fd(int fd) {
  ::_close(fd);
}

}  // namespace engine

#endif  // _WIN32
