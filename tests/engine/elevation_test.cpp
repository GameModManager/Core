// Elevated launch planning: argv construction, fakeroot detection, the option set
// the executables form offers, and the cancel/failure classification.
//
// No test in this file executes pkexec, osascript or fakeroot. Everything here
// asserts the argv that *would* be exec'd - running the helper would raise a
// real authorisation prompt and ask a real person for a password. The fakeroot
// and privilege-helper probes are injected so both the installed and missing
// branches are deterministic instead of depending on whether the machine running
// the suite happens to have them installed.
#include "engine/deploy/launch/elevation.h"
#include "ui/widgets/executables_entry.h"

#include <QApplication>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using engine::Elevation;
using engine::ElevationOutcome;

// Catch2 needs to be able to print these to name a failing REQUIRE.
std::ostream &operator<<(std::ostream &os, Elevation e) {
  return os << engine::elevation_to_string(e);
}
std::ostream &operator<<(std::ostream &os, ElevationOutcome o) {
  switch (o) {
    case ElevationOutcome::Success:
      return os << "Success";
    case ElevationOutcome::Cancelled:
      return os << "Cancelled";
    case ElevationOutcome::Failed:
      return os << "Failed";
  }
  return os << "?";
}

namespace {

// RAII so a failing REQUIRE cannot leave the probe replaced for the next test.
struct FakerootProbe {
  explicit FakerootProbe(bool present)
      : present_(present) {
    engine::set_fakeroot_probe([this] { return present_; });
  }
  ~FakerootProbe() { engine::set_fakeroot_probe({}); }
  bool present_;
};

// Same contract for the privilege helper: this one answers "is it installed",
// never "does this platform have one" - a platform with no implemented
// mechanism stays unavailable whatever the probe says.
struct HelperProbe {
  explicit HelperProbe(bool present)
      : present_(present) {
    engine::set_privilege_helper_probe([this] { return present_; });
  }
  ~HelperProbe() { engine::set_privilege_helper_probe({}); }
  bool present_;
};

// A QApplication for a case that builds widgets, owned by the case's scope.
//
// Deliberately a local rather than a function-local static: a QApplication
// destroyed from exit() segfaults inside qt_call_post_routines(), because by
// then the GUI library's own state is already partly finalised. Scoped to the
// case, it is torn down in reverse construction order while everything it walks
// is still alive, and the next case - its own process under ctest, the same
// process when the binary is run directly - simply builds another.
struct TestApp {
  int argc      = 1;
  char name[16] = "elevation_test";
  char *argv[2] = {name, nullptr};
  std::optional<QApplication> app;

  TestApp() { app.emplace(argc, argv); }
};

// The elevation combo of a form holding a single entry with this binary.
QVector<ui::Executables::ElevationOption> elevation_options_for(const char *path) {
  ui::Executables::Entry e;
  e.path  = QString::fromUtf8(path);
  e.title = "Game";
  ui::Executables::ContentWidget form(std::filesystem::path{}, {}, {e}, {}, nullptr);
  return form.elevation_options();
}

}  // namespace

TEST_CASE("elevation round-trips through the persisted string", "[engine][elevation]") {
  REQUIRE(engine::parse_elevation("") == Elevation::None);
  REQUIRE(engine::parse_elevation("fakeroot") == Elevation::Fakeroot);
  REQUIRE(engine::parse_elevation("root") == Elevation::Root);
  // A value from a newer build must not make a game launch unelevated-but-wrong.
  REQUIRE(engine::parse_elevation("sudo") == Elevation::None);

  REQUIRE(engine::elevation_to_string(Elevation::None).empty());
  REQUIRE(engine::elevation_to_string(Elevation::Fakeroot) == "fakeroot");
  REQUIRE(engine::elevation_to_string(Elevation::Root) == "root");

  for (auto e : {Elevation::None, Elevation::Fakeroot, Elevation::Root}) {
    REQUIRE(engine::parse_elevation(engine::elevation_to_string(e)) == e);
  }
}

TEST_CASE("fakeroot option is enabled when fakeroot is installed", "[engine][elevation]") {
  FakerootProbe probe(true);
  REQUIRE(engine::fakeroot_available());
  REQUIRE(engine::elevation_supported(Elevation::Fakeroot));

  const auto plan = engine::plan_elevation(Elevation::Fakeroot, "/games/skyrim/game", {});
  REQUIRE(plan.supported);
  REQUIRE(plan.argv.size() == 2);
  REQUIRE(plan.argv[1] == "/games/skyrim/game");
  // argv[0] is the fakeroot binary, resolved so the launch sites' exists() and
  // execvp() find it. Under the injected probe the real PATH is still consulted
  // for the path, so only assert the basename.
  REQUIRE(plan.argv[0].substr(plan.argv[0].find_last_of('/') + 1) == "fakeroot");
}

TEST_CASE("fakeroot option is disabled with a tooltip naming the reason",
          "[engine][elevation]") {
  FakerootProbe probe(false);
  REQUIRE_FALSE(engine::fakeroot_available());
  REQUIRE_FALSE(engine::elevation_supported(Elevation::Fakeroot));

  // The reason must survive to the caller, not degrade to an empty failure.
  const auto plan = engine::plan_elevation(Elevation::Fakeroot, "/games/skyrim/game", {});
  REQUIRE_FALSE(plan.supported);
  REQUIRE_FALSE(plan.reason.empty());
  REQUIRE(plan.argv.empty());
  REQUIRE(plan.reason.find("fakeroot") != std::string::npos);
  REQUIRE(plan.reason.find("not installed") != std::string::npos);

  // The disabled option's tooltip has to say the same thing, or the user is
  // left guessing why the choice is greyed out.
  const std::string tip = engine::fakeroot_unavailable_tooltip();
  REQUIRE(tip.find("fakeroot is not installed") != std::string::npos);
  REQUIRE(tip.find("Install") != std::string::npos);
}

TEST_CASE("no elevation leaves the argv untouched", "[engine][elevation]") {
  const std::vector<std::string> args = {"-skipintro", "--arg with space"};
  const auto plan = engine::plan_elevation(Elevation::None, "/games/skyrim/game", args);

  REQUIRE(plan.supported);
  REQUIRE(plan.argv.size() == 3);
  REQUIRE(plan.argv[0] == "/games/skyrim/game");
  REQUIRE(plan.argv[1] == "-skipintro");
  // An argument containing a space stays exactly one argv element. Nothing is
  // ever re-split or joined into a command line.
  REQUIRE(plan.argv[2] == "--arg with space");
}

TEST_CASE("elevation wraps the executable without building a shell string",
          "[engine][elevation]") {
  FakerootProbe probe(true);
  const std::vector<std::string> args = {"-a", "b; rm -rf /", "$(id)", "`whoami`"};

  const auto plan = engine::plan_elevation(Elevation::Fakeroot, "/games/my game/bin", args);
  REQUIRE(plan.supported);

  // Every dangerous-looking argument survives as its own discrete argv element,
  // which is what proves nothing was pasted into a shell command line: a shell
  // string would have needed quoting and escaping here.
  REQUIRE(plan.argv.size() == args.size() + 2);
  for (size_t i = 0; i < args.size(); ++i)
    REQUIRE(plan.argv[i + 2] == args[i]);
  // A path with a space survives intact too.
  REQUIRE(plan.argv[1] == "/games/my game/bin");
}

TEST_CASE("a relative executable is made absolute for the wrapper",
          "[engine][elevation]") {
  FakerootProbe probe(true);
  // pkexec and fakeroot exec the next argv element themselves and do not search
  // PATH or resolve a relative path for it, so a relative executable would be
  // looked up in the wrapper's cwd instead of the game's.
  const auto plan = engine::plan_elevation(Elevation::Fakeroot, "game", {});
  REQUIRE(plan.supported);
  REQUIRE(plan.argv.size() == 2);
  REQUIRE(plan.argv[1].front() == '/');
}

TEST_CASE("elevation choice round-trips through the executable entry model",
          "[ui][elevation]") {
  ui::Executables::Entry e;
  e.path      = "bin/game";
  e.title     = "Game";
  e.elevation = "fakeroot";

  const auto back = ui::Executables::Entry::fromJson(e.toJson());
  REQUIRE(back.elevation == "fakeroot");
  REQUIRE(back.path == e.path);
  REQUIRE(back.title == e.title);

  // A config written before the field existed has no "elev" key and must load
  // as "no elevation" rather than as fakeroot or root.
  QJsonObject legacy;
  legacy["path"] = "bin/game";
  REQUIRE(ui::Executables::Entry::fromJson(legacy).elevation.isEmpty());

  for (const char *value : {"", "fakeroot", "root"}) {
    ui::Executables::Entry r;
    r.elevation = QString::fromUtf8(value);
    REQUIRE(ui::Executables::Entry::fromJson(r.toJson()).elevation ==
            QString::fromUtf8(value));
  }
}

TEST_CASE("a dismissed authorisation prompt is a cancellation, not a failure",
          "[engine][elevation]") {
  REQUIRE(engine::classify_elevation_exit(0, Elevation::Root) == ElevationOutcome::Success);

#ifdef GMM_PLATFORM_LINUX
  // polkit's documented contract: 126 is the user dismissing the dialog.
  REQUIRE(engine::classify_elevation_exit(126, Elevation::Root) ==
          ElevationOutcome::Cancelled);
  // 127 is "not authorised / authorization could not be obtained" - a real
  // failure, e.g. no polkit agent could serve a prompt.
  REQUIRE(engine::classify_elevation_exit(127, Elevation::Root) ==
          ElevationOutcome::Failed);
  REQUIRE(engine::classify_elevation_exit(1, Elevation::Root) == ElevationOutcome::Failed);
#elif defined(GMM_PLATFORM_MACOS)
  // osascript collapses every AppleScript error to exit 1, so a dismissal is
  // only recognisable from the error text.
  REQUIRE(engine::classify_elevation_exit(1, Elevation::Root,
                                         "execution error: User canceled. (-128)") ==
          ElevationOutcome::Cancelled);
  REQUIRE(engine::classify_elevation_exit(1, Elevation::Root, "command not found") ==
          ElevationOutcome::Failed);
#endif

  // fakeroot raises no prompt, so its non-zero exits are always failures - a
  // 126 there must not be misread as "the user cancelled".
  REQUIRE(engine::classify_elevation_exit(126, Elevation::Fakeroot) ==
          ElevationOutcome::Failed);
}

TEST_CASE("elevated launch only reports success when a command was built",
          "[engine][elevation]") {
  FakerootProbe probe(false);
  const auto plan = engine::plan_elevation(Elevation::Fakeroot, "/games/skyrim/game", {});
  // An unsupported plan carries no argv at all, so a caller that only checked
  // argv.size() would have nothing to launch and must not claim it did.
  REQUIRE_FALSE(plan.supported);
  REQUIRE(plan.argv.empty());
}

TEST_CASE("the elevated option wraps the executable without building a shell string",
          "[engine][elevation]") {
#ifdef GMM_PLATFORM_WINDOWS
  // No mechanism is implemented here, so there is no argv to inspect - the
  // honest-unavailable contract is the next test's subject.
  REQUIRE_FALSE(engine::elevation_supported(Elevation::Root));
#else
  HelperProbe probe(true);
  const std::vector<std::string> args = {"-a", "b; rm -rf /", "$(id)", "`whoami`"};

  const auto plan = engine::plan_elevation(Elevation::Root, "/games/my game/bin", args);
  REQUIRE(plan.supported);

  // argv[0] is the privilege helper, never the executable: if the executable
  // were first, the game would run unelevated while the form claimed otherwise.
  // It is resolved to an absolute path when the helper is installed and stays a
  // bare name when it is not, so only the basename is asserted.
#ifdef GMM_PLATFORM_MACOS
  // osascript takes one script string, but the executable and every argument
  // travel in its own argv and are quoted inside the script - nothing is
  // pasted into a shell command line here.
  REQUIRE(plan.argv[0] == "osascript");
  REQUIRE(plan.argv[1] == "-e");
  REQUIRE(plan.argv.size() == args.size() + 5);
  REQUIRE(plan.argv[4] == "/games/my game/bin");
  for (size_t i = 0; i < args.size(); ++i)
    REQUIRE(plan.argv[i + 5] == args[i]);
#else
  REQUIRE(plan.argv[0].substr(plan.argv[0].find_last_of('/') + 1) == "pkexec");
  REQUIRE(plan.argv.size() == args.size() + 2);
  REQUIRE(plan.argv[1] == "/games/my game/bin");
  for (size_t i = 0; i < args.size(); ++i)
    REQUIRE(plan.argv[i + 2] == args[i]);
#endif
#endif
}

TEST_CASE("an unavailable privilege helper is refused, not run unelevated",
          "[engine][elevation]") {
  // A platform with no implemented mechanism cannot be talked into having one.
  HelperProbe probe(false);
  REQUIRE_FALSE(engine::elevation_supported(Elevation::Root));

  const auto plan = engine::plan_elevation(Elevation::Root, "/games/skyrim/game", {});
  // The refusal has to be loud and empty-handed: a plan that quietly carried
  // the bare executable would launch the game and let the user believe the
  // setting took effect.
  REQUIRE_FALSE(plan.supported);
  REQUIRE_FALSE(plan.reason.empty());
  REQUIRE(plan.argv.empty());
}

TEST_CASE("the elevation combo offers no elevation, the platform prompt and fakeroot",
          "[ui][elevation]") {
  TestApp app;
  const auto options = elevation_options_for("bin/game");

#ifdef GMM_PLATFORM_LINUX
  REQUIRE(options.size() == 3);
#else
  // fakeroot fakes a Unix uid by interposing libc, which is only meaningful
  // where libc loads the process, so it is not offered elsewhere.
  REQUIRE(options.size() == 2);
#endif
  REQUIRE(options.first().label == "--- None ---");
  REQUIRE(options.first().value.isEmpty());

  // Exactly one elevated option, on every platform, labelled by what it does
  // rather than by the mechanism behind it.
  int elevated  = -1;
  int fakeroot  = -1;
  for (int i = 0; i < options.size(); ++i) {
    if (options[i].value == "root")
      elevated = i;
    if (options[i].value == "fakeroot")
      fakeroot = i;
  }
  REQUIRE(elevated >= 0);
  REQUIRE(options[elevated].label == "Run with elevated privileges");
#ifdef GMM_PLATFORM_LINUX
  REQUIRE(fakeroot >= 0);
  REQUIRE(options[fakeroot].label == "fakeroot (no password)");
#else
  REQUIRE(fakeroot < 0);
#endif

  // Every offered option is a value the engine and an existing config already
  // understand, so a selection round-trips through the entry and through the
  // engine instead of degrading to "no elevation" on the next save.
  for (const auto &option : options) {
    INFO(option.label.toStdString());
    ui::Executables::Entry entry;
    entry.elevation = option.value;
    REQUIRE(ui::Executables::Entry::fromJson(entry.toJson()).elevation == option.value);
    REQUIRE(engine::elevation_to_string(
                engine::parse_elevation(option.value.toStdString())) ==
            option.value.toStdString());
  }
}

TEST_CASE("the elevation options are the same for every executable", "[ui][elevation]") {
  TestApp app;
  // What elevates a launch is decided by the platform, not by what the file is:
  // a Windows binary, a native one and a script all get the same choices, and
  // the combo's items are not rebuilt per selected entry.
  const auto options = elevation_options_for("bin/game.exe");
  REQUIRE(options == elevation_options_for("bin/game"));
  REQUIRE(options == elevation_options_for("bin/game.sh"));
}

TEST_CASE("the fakeroot option follows whether fakeroot is installed", "[ui][elevation]") {
#ifdef GMM_PLATFORM_LINUX
  for (const bool present : {true, false}) {
    // The probe is in scope before the form is built: the combo reads it while
    // it populates, which is the only time the greyed-out state is decided.
    FakerootProbe probe(present);
    TestApp app;
    const auto options = elevation_options_for("bin/game");
    const auto fakeroot =
        std::find_if(options.cbegin(), options.cend(), [](const auto &option) {
          return option.value == "fakeroot";
        });
    REQUIRE(fakeroot != options.cend());
    REQUIRE(fakeroot->enabled == present);
  }
#endif
}
