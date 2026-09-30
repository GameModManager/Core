#include "engine/deploy/launch/elevation.h"

#include <cstdlib>
#include <optional>

#include <unistd.h>

namespace engine {
namespace {

// Resolves an executable name against PATH. Used to answer "is the privilege
// helper installed" without running it - running pkexec to find out would put a
// password prompt on screen. Returns the resolved absolute path, because the
// launch sites stat() the program they are about to run and a bare name would
// not resolve there.
std::optional<std::filesystem::path> found_on_path(const std::string &name) {
  const char *path_env = std::getenv("PATH");
  if (!path_env)
    return std::nullopt;

  std::string path_env_str(path_env);
  std::size_t start = 0;
  while (start <= path_env_str.size()) {
    const std::size_t end = path_env_str.find(':', start);
    const std::string dir = path_env_str.substr(
        start, end == std::string::npos ? std::string::npos : end - start);

    // An empty PATH element means the current directory.
    const std::filesystem::path candidate =
        (dir.empty() ? std::filesystem::path(".") : std::filesystem::path(dir)) / name;

    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec) &&
        access(candidate.c_str(), X_OK) == 0)
      return candidate;

    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return std::nullopt;
}

// Absolute path of a privilege helper. The caller has already established the
// helper exists (elevation_supported), so a failed re-resolve can only mean
// PATH changed underneath us; the bare name still execs correctly via execvp.
std::filesystem::path helper_path(const std::string &name) {
  if (const auto p = found_on_path(name))
    return *p;
  return std::filesystem::path(name);
}

std::function<bool()> &fakeroot_probe() {
  static std::function<bool()> probe;
  return probe;
}

// fakeroot is installed per machine and does not change while we run, so the
// answer is resolved on first use and kept. The executables form rebuilds its
// combo items on every selection change; scanning PATH each time would be a
// stat storm for an answer that cannot change.
bool probe_fakeroot() {
  static const bool available = found_on_path("fakeroot").has_value();
  return available;
}

// The helper that raises the platform's own privilege prompt: polkit's pkexec
// on Linux, osascript on macOS. Empty means this build has no mechanism at all,
// which is the honest answer - the caller reports the elevation unavailable
// rather than launching unelevated behind the user's back.
const char *privilege_helper_name() {
#ifdef GMM_PLATFORM_LINUX
  return "pkexec";
#elif defined(GMM_PLATFORM_MACOS)
  return "osascript";
#else
  // Windows would call ShellExecuteEx with the "runas" verb to raise the UAC
  // prompt. That path is deliberately not written yet, so there is no helper to
  // look for here.
  return "";
#endif
}

std::function<bool()> &privilege_helper_probe() {
  static std::function<bool()> probe;
  return probe;
}

// Installed per machine and cached for the same reason as fakeroot: the answer
// cannot change while we run, so a PATH scan per evaluation buys nothing.
bool probe_privilege_helper() {
  static const bool available = [] {
    const char *name = privilege_helper_name();
    return name[0] != '\0' && found_on_path(name).has_value();
  }();
  return available;
}

// AppleScript source for the macOS privilege prompt. The command and every
// argument arrive in osascript's own argv, and the script joins them with
// `quoted form of`, which Apple documents as returning "a string in a form
// that's safe from further interpretation by the shell, regardless of its
// contents". So the quoting is done by the OS, not by a shell string built here.
const char *const kMacPrivilegeScript =
    "on run argv\n"
    "set cmdText to quoted form of item 1 of argv\n"
    "repeat with a from 2 to (count of argv)\n"
    "set cmdText to cmdText & \" \" & quoted form of (item a of argv)\n"
    "end repeat\n"
    "do shell script cmdText with administrator privileges\n"
    "end run";

}  // namespace

Elevation parse_elevation(const std::string &value) {
  if (value == "fakeroot")
    return Elevation::Fakeroot;
  if (value == "root")
    return Elevation::Root;
  return Elevation::None;
}

std::string elevation_to_string(Elevation elevation) {
  switch (elevation) {
    case Elevation::Fakeroot:
      return "fakeroot";
    case Elevation::Root:
      return "root";
    case Elevation::None:
      break;
  }
  return "";
}

bool elevation_supported(Elevation elevation) {
  if (elevation == Elevation::None)
    return true;
  if (elevation == Elevation::Fakeroot)
    return fakeroot_available();

  // No helper on this platform is a hard no, whatever a probe says: there is
  // nothing installed to probe for.
  if (privilege_helper_name()[0] == '\0')
    return false;
  const auto &probe = privilege_helper_probe();
  return probe ? probe() : probe_privilege_helper();
}

bool fakeroot_available() {
  const auto &probe = fakeroot_probe();
  return probe ? probe() : probe_fakeroot();
}

void set_fakeroot_probe(std::function<bool()> probe) { fakeroot_probe() = std::move(probe); }

void set_privilege_helper_probe(std::function<bool()> probe) {
  privilege_helper_probe() = std::move(probe);
}

const char *fakeroot_unavailable_tooltip() {
  return "fakeroot is not installed on this system, so this option cannot be used.\n"
         "Install the \"fakeroot\" package (Debian/Ubuntu, Arch, Fedora) and restart\n"
         "GameModManager.";
}

ElevationPlan plan_elevation(Elevation elevation,
                             const std::filesystem::path &executable,
                             const std::vector<std::string> &args) {
  ElevationPlan plan;
  if (elevation == Elevation::None) {
    plan.argv.push_back(executable.string());
    for (const auto &a : args)
      plan.argv.push_back(a);
    return plan;
  }

  if (!elevation_supported(elevation)) {
    plan.supported = false;
    if (elevation == Elevation::Fakeroot) {
      plan.reason =
          "fakeroot is not installed on this system. Install the \"fakeroot\" package to "
          "launch this executable under fakeroot.";
    } else {
#ifdef GMM_PLATFORM_WINDOWS
      plan.reason =
          "Running this executable as administrator is not supported on Windows yet.";
#elif defined(GMM_PLATFORM_MACOS)
      plan.reason = "osascript, which raises the macOS administrator prompt, was not found.";
#else
      plan.reason =
          "pkexec, which raises the desktop environment's privilege prompt, was not "
          "found. Install polkit (package \"policykit-1\" on Debian/Ubuntu, "
          "\"polkit\" on Fedora, \"polkit\" on Arch) to launch this executable as root.";
#endif
    }
    return plan;
  }

  // A relative or bare name is resolved against the launch cwd by execv, but a
  // wrapper needs an absolute path to find: pkexec and fakeroot both exec the
  // next argv element without searching PATH for it.
  std::string exe = executable.string();
  if (exe.empty() || exe.front() != '/') {
    std::error_code ec;
    const auto abs = std::filesystem::absolute(executable, ec);
    if (!ec)
      exe = abs.string();
  }

  if (elevation == Elevation::Root) {
#ifdef GMM_PLATFORM_MACOS
    // The "-" after the -e statements separates osascript's own options from
    // the arguments forwarded to the run handler, so the executable and its
    // arguments travel through the process argv and are quoted inside the
    // script rather than interpolated into a shell command line here.
    plan.argv = {"osascript", "-e", kMacPrivilegeScript, "-", exe};
    for (const auto &a : args)
      plan.argv.push_back(a);
    return plan;
#else
    // Non-empty here: elevation_supported() already refused the plan on a
    // platform with no helper. Absolute, because the launch sites stat()
    // argv[0] and exec the next element without a PATH search of their own.
    plan.argv = {helper_path(privilege_helper_name()).string(), exe};
    for (const auto &a : args)
      plan.argv.push_back(a);
    return plan;
#endif
  }

  plan.argv = {helper_path("fakeroot").string(), exe};
  for (const auto &a : args)
    plan.argv.push_back(a);
  return plan;
}

ElevationOutcome classify_elevation_exit(int exit_code, Elevation elevation,
                                         const std::string &stderr_text) {
  if (exit_code == 0)
    return ElevationOutcome::Success;
  if (elevation != Elevation::Root)
    return ElevationOutcome::Failed;

#ifdef GMM_PLATFORM_LINUX
  // polkit's documented contract: 126 is the user dismissing the dialog.
  if (exit_code == 126)
    return ElevationOutcome::Cancelled;
#elif defined(GMM_PLATFORM_MACOS)
  // osascript collapses every AppleScript error to exit 1. A dismissal is
  // error -128 ("User canceled."), which only the message identifies.
  if (stderr_text.find("User canceled") != std::string::npos)
    return ElevationOutcome::Cancelled;
#else
  (void)stderr_text;
#endif

  return ElevationOutcome::Failed;
}

}  // namespace engine
