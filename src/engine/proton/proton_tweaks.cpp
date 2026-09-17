#include "engine/proton/proton_tweaks.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#ifdef __linux__
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QString>
#include <QStringList>
#endif

namespace fs = std::filesystem;

namespace engine::proton {

namespace {

// Windows scale steps (100/125/150/175/200%).
constexpr int kDpiSteps[] = {96, 120, 144, 168, 192};

// Snap a raw DPI value to the nearest Windows step.
int snap_dpi(int raw) {
  int best = kDpiSteps[0];
  for (int step : kDpiSteps) {
    if (std::abs(raw - step) < std::abs(raw - best)) best = step;
  }
  return best;
}

#ifdef __linux__

constexpr const char *kDesktopKey = "HKCU\\Control Panel\\Desktop";
constexpr const char *kFontsKey = "HKCU\\Software\\Wine\\Fonts";

// Longer timeout for protontricks (downloads + installs corefonts).
constexpr int kRegTimeoutMs = 60000;
constexpr int kTricksTimeoutMs = 600000;

fs::path find_on_path(const std::string &name) {
  const char *path_env = std::getenv("PATH");
  if (!path_env) return {};
  std::istringstream ss(path_env);
  std::string token;
  while (std::getline(ss, token, ':')) {
    auto candidate = fs::path(token) / name;
    std::error_code ec;
    if (fs::exists(candidate, ec)) return candidate;
  }
  return {};
}

// System wine binary (mirrors LinuxPlatform::find_wine without the Platform
// dependency - this module only gets a prefix + appid).
fs::path find_wine_binary() {
  auto wine = find_on_path("wine");
  if (!wine.empty()) return wine;
  for (const auto &c : {"/usr/bin/wine", "/usr/local/bin/wine",
                        "/opt/wine/bin/wine"}) {
    std::error_code ec;
    if (fs::exists(c, ec)) return c;
  }
  return {};
}

struct SyncResult {
  bool started = false;
  int exit_code = -1;
  QString output;
};

// Run a process synchronously with WINEPREFIX set. Never throws, never
// crashes on missing binaries: reports via `started`/`exit_code`.
SyncResult run_sync(const fs::path &program,
                    const std::vector<std::string> &args,
                    const fs::path &prefix, int timeout_ms) {
  SyncResult r;
  if (program.empty()) return r;
  QProcess proc;
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert("WINEPREFIX", QString::fromStdString(prefix.string()));
  env.insert("WINETRICKS_LATEST_VERSION_CHECK", "disabled");
  env.insert("WINEDEBUG", "-all");
  proc.setProcessEnvironment(env);
  QStringList qargs;
  for (const auto &a : args) qargs << QString::fromStdString(a);
  proc.start(QString::fromStdString(program.string()), qargs);
  if (!proc.waitForStarted(kRegTimeoutMs)) return r;
  r.started = true;
  if (!proc.waitForFinished(timeout_ms)) {
    proc.kill();
    proc.waitForFinished(5000);
    return r;
  }
  r.exit_code = proc.exitCode();
  r.output = QString::fromLocal8Bit(proc.readAllStandardOutput());
  return r;
}

// Parse `wine reg query` output: the line whose first token is `name`,
// value is the last token (hex DWORDs like 0x00000002 stay raw here).
std::optional<std::string> query_reg_value(const fs::path &wine,
                                           const fs::path &prefix,
                                           const std::string &key,
                                           const std::string &name) {
  auto res = run_sync(wine, {"reg", "query", key, "/v", name}, prefix,
                      kRegTimeoutMs);
  if (!res.started || res.exit_code != 0) return std::nullopt;
  const QString qname = QString::fromStdString(name);
  for (const auto &line : res.output.split('\n')) {
    const auto tokens =
        line.trimmed().split(QChar(' '), Qt::SkipEmptyParts);
    if (tokens.size() >= 3 && tokens.front() == qname)
      return tokens.back().toStdString();
  }
  return std::nullopt;
}

// Normalize a reg value to int: 0x-prefixed hex or plain decimal.
std::optional<int> parse_reg_number(const std::string &raw) {
  try {
    size_t pos = 0;
    int base = 10;
    std::string s = raw;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      base = 16;
      s = s.substr(2);
    }
    int v = std::stoi(s, &pos, base);
    if (pos != s.size()) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

// Empty string = success, otherwise a short error description.
std::string set_reg_value(const fs::path &wine, const fs::path &prefix,
                          const std::string &key, const std::string &name,
                          const std::string &type, const std::string &data) {
  auto res = run_sync(wine,
                      {"reg", "add", key, "/v", name, "/t", type, "/d", data,
                       "/f"},
                      prefix, kRegTimeoutMs);
  if (!res.started) return "could not start wine reg";
  if (res.exit_code != 0)
    return "wine reg add failed for " + name + " (exit " +
           std::to_string(res.exit_code) + ")";
  return {};
}

std::string check_prefix(const fs::path &prefix) {
  if (prefix.empty()) return "no Proton prefix configured";
  std::error_code ec;
  if (!fs::exists(prefix, ec)) return "prefix not found: " + prefix.string();
  return {};
}

std::string ensure_corefonts_impl(const fs::path &prefix, uint32_t appid) {
  if (auto e = check_prefix(prefix); !e.empty()) return e;
  // Idempotent: winetricks logs every installed verb.
  std::ifstream log(prefix / "winetricks.log");
  std::string line;
  while (std::getline(log, line)) {
    if (line == "corefonts") return {};
  }
  if (appid == 0) return "corefonts missing but no Steam appid for routing";
  auto tricks = find_on_path("protontricks");
  if (tricks.empty()) {
    tricks = find_on_path("winetricks");
    if (tricks.empty()) return "neither protontricks nor winetricks found";
  }
  std::vector<std::string> args;
  if (tricks.filename() == "protontricks")
    args = {"--no-term", std::to_string(appid), "corefonts"};
  else
    args = {"-q", "corefonts"};
  auto res = run_sync(tricks, args, prefix, kTricksTimeoutMs);
  if (!res.started) return "could not start winetricks backend";
  if (res.exit_code != 0)
    return "corefonts install failed (exit " +
           std::to_string(res.exit_code) + ")";
  return {};
}

std::string ensure_font_smoothing_impl(const fs::path &prefix) {
  if (auto e = check_prefix(prefix); !e.empty()) return e;
  auto wine = find_wine_binary();
  if (wine.empty()) return "wine binary not found on PATH";
  struct Want {
    const char *name;
    const char *type;
    bool is_string;
    int number;
  };
  // RGB ClearType: FontSmoothing=2 (REG_SZ), type/gamma/orientation DWORDs.
  constexpr Want kWant[] = {
      {"FontSmoothing", "REG_SZ", true, 2},
      {"FontSmoothingType", "REG_DWORD", false, 2},
      {"FontSmoothingGamma", "REG_DWORD", false, 1400},
      {"FontSmoothingOrientation", "REG_DWORD", false, 1},
  };
  for (const auto &w : kWant) {
    bool correct = false;
    if (auto cur = query_reg_value(wine, prefix, kDesktopKey, w.name)) {
      if (w.is_string)
        correct = (*cur == std::to_string(w.number));
      else if (auto n = parse_reg_number(*cur))
        correct = (*n == w.number);
    }
    if (!correct) {
      if (auto e = set_reg_value(wine, prefix, kDesktopKey, w.name, w.type,
                                 std::to_string(w.number));
          !e.empty())
        return e;
    }
  }
  return {};
}

std::string ensure_dpi_impl(const fs::path &prefix, int target_dpi) {
  if (auto e = check_prefix(prefix); !e.empty()) return e;
  auto wine = find_wine_binary();
  if (wine.empty()) return "wine binary not found on PATH";
  bool correct = false;
  if (auto cur = query_reg_value(wine, prefix, kFontsKey, "LogPixels")) {
    if (auto n = parse_reg_number(*cur)) correct = (*n == target_dpi);
  }
  if (!correct) {
    return set_reg_value(wine, prefix, kFontsKey, "LogPixels", "REG_DWORD",
                         std::to_string(target_dpi));
  }
  return {};
}

#endif  // __linux__

}  // namespace

TweakResult apply_proton_tweaks(const fs::path &prefix, uint32_t steam_appid,
                                int force_dpi) {
  TweakResult r;
#ifdef __linux__
  if (auto e = check_prefix(prefix); !e.empty()) {
    r.error = e;
    return r;
  }
  r.detected_dpi = force_dpi > 0 ? snap_dpi(force_dpi) : detect_system_dpi();
  std::string e = ensure_corefonts_impl(prefix, steam_appid);
  r.corefonts_installed = e.empty();
  if (!e.empty()) r.error = e;
  e = ensure_font_smoothing_impl(prefix);
  r.font_smoothing_set = e.empty();
  if (!e.empty() && r.error.empty()) r.error = e;
  e = ensure_dpi_impl(prefix, r.detected_dpi);
  r.dpi_set = e.empty();
  if (!e.empty() && r.error.empty()) r.error = e;
#else
  (void)prefix;
  (void)steam_appid;
  (void)force_dpi;
  r.error = "Proton tweaks are only supported on Linux";
#endif
  return r;
}

bool ensure_corefonts(const fs::path &prefix, uint32_t steam_appid) {
#ifdef __linux__
  return ensure_corefonts_impl(prefix, steam_appid).empty();
#else
  (void)prefix;
  (void)steam_appid;
  return false;
#endif
}

bool ensure_font_smoothing(const fs::path &prefix, uint32_t) {
#ifdef __linux__
  return ensure_font_smoothing_impl(prefix).empty();
#else
  (void)prefix;
  return false;
#endif
}

bool ensure_dpi(const fs::path &prefix, uint32_t, int force_dpi) {
#ifdef __linux__
  int target = force_dpi > 0 ? snap_dpi(force_dpi) : detect_system_dpi();
  return ensure_dpi_impl(prefix, target).empty();
#else
  (void)prefix;
  (void)force_dpi;
  return false;
#endif
}

int detect_system_dpi() {
#ifdef __linux__
  if (auto *gui =
          qobject_cast<QGuiApplication *>(QGuiApplication::instance())) {
    if (QScreen *screen = gui->primaryScreen()) {
      int raw = static_cast<int>(std::lround(screen->logicalDotsPerInch()));
      if (raw > 0) return snap_dpi(raw);
    }
  }
#endif
  return 96;
}

}  // namespace engine::proton
