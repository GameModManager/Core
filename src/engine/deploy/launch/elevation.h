#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace engine {

// How a launched executable asks for extra privilege. Persisted per executable
// in the instance's executables array as the strings "", "fakeroot" and "root".
enum class Elevation {
  None,     // no wrapping (empty string)
  Fakeroot, // fakeroot <exe> <args...> - unprivileged, fakes uid 0
  Root,     // the desktop environment's own privilege prompt
};

// Maps the persisted string onto the enum. Anything unrecognised (including a
// value written by a newer build) degrades to None so a bad config never makes
// a game launch unelevated-but-wrong.
Elevation parse_elevation(const std::string &value);
std::string elevation_to_string(Elevation elevation);

enum class ElevationOutcome {
  Success,
  Cancelled, // the OS prompt was dismissed - not a failure
  Failed,
};

// The result of planning an elevated launch: the argv to exec, or the reason
// there is none. Planning never executes anything.
struct ElevationPlan {
  bool supported = true;
  std::string reason;             // why unsupported; empty when supported
  std::vector<std::string> argv;  // complete argv including the wrapper in [0]
};

// Builds the argv that launches `executable` under the requested elevation.
//
// The argv is assembled element by element: no path and no argument is ever
// concatenated into a shell command string. The only platform that cannot avoid
// a shell is macOS, where the OS privilege API takes a single script string -
// there the values are handed to osascript's own argv and quoted by the OS.
ElevationPlan plan_elevation(Elevation elevation,
                             const std::filesystem::path &executable,
                             const std::vector<std::string> &args);

// True when fakeroot is installed and executable. Scans PATH once and caches
// the answer; the executables form is rebuilt many times per session, so this
// must not touch the filesystem per repaint.
bool fakeroot_available();

// The tooltip shown on the fakeroot option when it is unavailable. The user has
// to be told why the option is greyed out rather than left to guess.
const char *fakeroot_unavailable_tooltip();

// Test seam: replaces the fakeroot probe so both the installed and missing
// branches are deterministic regardless of the machine running the suite.
// Pass an empty function to restore PATH scanning.
void set_fakeroot_probe(std::function<bool()> probe);

// True when this platform has a desktop-environment privilege prompt at all.
// Windows is not implemented yet, so root is honestly reported as unavailable
// there instead of silently running unelevated.
bool elevation_supported(Elevation elevation);

// Classifies how an elevation helper process ended.
//
// On Linux pkexec reserves distinct exit codes (polkit's documented contract):
// 126 when the user dismissed the authentication dialog, 127 when
// authorization could not be obtained or an error occurred. The dismissal is a
// cancellation, not something to report as a failure.
//
// macOS has no such code. osascript reports every AppleScript error as exit 1,
// so the dialog dismissal is only distinguishable from the error text - hence
// the stderr argument. A failure whose text does not identify a cancellation
// stays a failure.
ElevationOutcome classify_elevation_exit(int exit_code, Elevation elevation,
                                         const std::string &stderr_text = {});

}  // namespace engine
