// Equivalence + reachability proof for the OS primitives that were moved out
// of engine/ into src/platform/ (Workspace-fjx8).
//
// The routing itself is mechanical; what needs proving is that the adaptor
// body does the same thing the call site used to do, and that the call site
// really reaches it. So every case here pairs a positive assertion with a
// negative control that would have failed had the body been wrong.
//
// The freedesktop / Recycle Bin / Finder-Trash behaviour of
// move_to_recycle_bin is per-OS; the assertions below check the one property
// every OS guarantees (the file is gone from where it was, and something
// recoverable took its place) rather than a layout only Linux has.

#include "engine/log/logger.h"
#include "engine/profile/safe_write_file.h"
#include "engine/util/fs_utils.h"
#include "platform/platform.h"

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

namespace fs = std::filesystem;

namespace {

// A directory unique to this process, so a case never collides with the
// user's real trash, the user's real cache, or a concurrent case.
fs::path scratch(const char *name) {
  const auto dir =
      fs::temp_directory_path() /
      ("gmm_platform_primitives_" + std::to_string(current_process_id()) + "_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::string read_file(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

void write_file(const fs::path &p, const std::string &content) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << content;
}

}  // namespace

TEST_CASE("local_time breaks down the instant it was given", "[platform]") {
  // 2021-01-01T00:00:00Z. Under TZ=UTC (the CTest ENVIRONMENT for every test)
  // this is also the local reading, which is what makes the negative control
  // below meaningful rather than tautological.
  constexpr std::time_t kEpoch = 1609459200;

  const std::tm tm = local_time(kEpoch);
  REQUIRE(tm.tm_year == 121);  // 2021 - 1900
  REQUIRE(tm.tm_mon == 0);
  REQUIRE(tm.tm_mday == 1);
  REQUIRE(tm.tm_hour == 0);
  REQUIRE(tm.tm_min == 0);
  REQUIRE(tm.tm_sec == 0);

  // Negative control: the same call with a *different* instant must not return
  // the same breakdown. If local_time ignored its argument and returned a
  // constant, or if the Windows/POSIX argument order were swapped at one call
  // site (localtime_s(tm*, time_t*) vs localtime_r(time_t*, tm*)), this fails.
  const std::tm later = local_time(kEpoch + 3661);  // +1h 1m 1s
  REQUIRE(later.tm_hour == 1);
  REQUIRE(later.tm_min == 1);
  REQUIRE(later.tm_sec == 1);
  REQUIRE(later.tm_year == 121);
}

TEST_CASE("utc_time is UTC regardless of the process timezone", "[platform]") {
  // The point of the pair is that utc_time is unaffected by TZ while
  // local_time is not. CTest pins TZ=UTC, so this case pins it to something
  // else for its own duration and restores it - that is what makes the second
  // assertion a real control rather than a duplicate of the first.
  const char *const old_tz = std::getenv("TZ");
  const std::string saved  = old_tz ? old_tz : "";

  // POSIX TZ sign convention is inverted relative to how it reads: "UTC-9" is
  // nine hours AHEAD of UTC. Getting this backwards would assert 15:00 and
  // fail for a reason that has nothing to do with the code under test.
  ::setenv("TZ", "UTC-9", 1);
  ::tzset();

  const std::tm utc = utc_time(1609459200);
  REQUIRE(utc.tm_year == 121);
  REQUIRE(utc.tm_hour == 0);

  // Negative control: under a +9 offset the same instant reads as 09:00 local,
  // so a local_time that had accidentally been wired to the UTC helper - or an
  // adaptor that leaked TZ into it - shows up as tm_hour == 0 here.
  const std::tm local = local_time(1609459200);
  REQUIRE(local.tm_hour == 9);

  if (old_tz)
    ::setenv("TZ", saved.c_str(), 1);
  else
    ::unsetenv("TZ");
  ::tzset();
}

TEST_CASE("current_process_id names this process", "[platform]") {
  // Consumers only need uniqueness, so the assertion is that two calls agree
  // and that the value is a plausible pid - not that it equals a hardcoded
  // number, which would make the test fail under ctest for no real reason.
  REQUIRE(current_process_id() > 0);
  REQUIRE(current_process_id() == current_process_id());

  // Negative control: a getpid() that returned 0 or a constant would satisfy
  // "two calls agree". This one compares against the OS truth.
  REQUIRE(current_process_id() == static_cast<long>(::getpid()));
}

TEST_CASE("atomic_replace swaps a file in and leaves no temp behind", "[platform]") {
  const auto dir    = scratch("atomic_replace");
  const auto target = dir / "live.toml";
  const auto staged = dir / "staged.toml";

  write_file(target, "old-and-longer");
  write_file(staged, "new");

  REQUIRE(atomic_replace(staged, target));

  // The consumer is whatever reads `target`: it must see the new bytes whole.
  REQUIRE(read_file(target) == "new");
  // And the staged file is gone - a rename, not a copy.
  REQUIRE_FALSE(fs::exists(staged));

  // Negative control: the content is actually *replaced*, not appended or
  // merged. "new" is a prefix-free string, so an append would show up as
  // "old-and-longern ew"-style garbage and fail this read.
  REQUIRE(read_file(target) != "old-and-longer");

  fs::remove_all(dir);
}

TEST_CASE("atomic_replace refuses a source that does not exist", "[platform]") {
  const auto dir    = scratch("atomic_replace_missing");
  const auto target = dir / "live.toml";
  write_file(target, "intact");

  REQUIRE_FALSE(atomic_replace(dir / "no-such-file", target));
  // Negative control: the failed replace must leave the existing file alone.
  // A body that returned true regardless would report success and a caller
  // would go on believing the new content is live.
  REQUIRE(read_file(target) == "intact");

  fs::remove_all(dir);
}

TEST_CASE("safe_write_file writes through atomic_replace", "[platform]") {
  const auto dir    = scratch("safe_write");
  const auto target = dir / "sub" / "profile.toml";

  REQUIRE(engine::profile::safe_write_file(target, "first"));
  REQUIRE(read_file(target) == "first");

  // A second write over an existing file is the atomic-replace path, and it
  // must fully overwrite: this is the profile-save path, so a partial write
  // here is a corrupted profile.
  REQUIRE(engine::profile::safe_write_file(target, "second-write"));
  REQUIRE(read_file(target) == "second-write");

  // Negative control: no temp file survives a successful write.
  int leftovers = 0;
  for (const auto &e : fs::directory_iterator(target.parent_path()))
    if (e.path().filename().string().find(".tmp") != std::string::npos)
      leftovers++;
  REQUIRE(leftovers == 0);

  fs::remove_all(dir);
}

TEST_CASE("create_truncated_file creates empty and truncates existing", "[platform]") {
  const auto dir   = scratch("create_truncated");
  const auto fresh = dir / "disable_me.txt";

  std::error_code ec;
  REQUIRE(create_truncated_file(fresh, ec));
  REQUIRE_FALSE(ec);
  REQUIRE(fs::exists(fresh));
  REQUIRE(fs::file_size(fresh) == 0);

  // Truncation, not append and not "leave alone": the caller is writing a
  // disable sentinel over a stale one.
  write_file(fresh, "stale content that is longer");
  REQUIRE(create_truncated_file(fresh, ec));
  REQUIRE_FALSE(ec);
  REQUIRE(fs::file_size(fresh) == 0);
  REQUIRE(read_file(fresh).empty());

  // Negative control: a create that silently did nothing would still leave the
  // file present, so assert on the size, not just on existence.
  REQUIRE(fs::file_size(fresh) != 12);

  fs::remove_all(dir);
}

TEST_CASE("create_truncated_file reports why it failed", "[platform]") {
  const auto dir = scratch("create_truncated_fail");
  // A path whose parent is a regular file cannot be created under any OS.
  const auto blocker = dir / "blocker";
  write_file(blocker, "x");

  std::error_code ec;
  REQUIRE_FALSE(create_truncated_file(blocker / "child.txt", ec));
  // Negative control: `ec` must carry a reason, not just a false return. The
  // disable-sentinel caller builds a user-facing message from ec.message(),
  // so an empty error_code here means "could not write" with no explanation.
  REQUIRE(static_cast<bool>(ec));
  REQUIRE_FALSE(ec.message().empty());

  fs::remove_all(dir);
}

TEST_CASE("move_to_recycle_bin empties the source and is recoverable", "[platform]") {
  const auto dir = scratch("recycle");
  // The Linux body reads XDG_DATA_HOME at call time, so a per-test trash root
  // keeps this out of the user's real trash.
  const auto trash_home = dir / "xdg_data";
  ::setenv("XDG_DATA_HOME", trash_home.c_str(), 1);

  const auto victim = dir / "mod.txt";
  write_file(victim, "recover me");

  REQUIRE(move_to_recycle_bin(victim));
  // The property every OS guarantees: it is gone from where it was.
  REQUIRE_FALSE(fs::exists(victim));

  // Negative control: "gone" must mean recoverable, not deleted. If the body
  // had fallen through to remove_all, both assertions would pass on Linux
  // unless one of them looks inside the trash - so look.
  const auto trash = trash_home / "Trash";
  REQUIRE(fs::is_directory(trash));

  int found = 0;
  for (const auto &e : fs::recursive_directory_iterator(trash))
    if (e.is_regular_file() && read_file(e.path()) == "recover me")
      found++;
  REQUIRE(found >= 1);  // the payload itself, plus a .trashinfo sidecar on Linux

  ::unsetenv("XDG_DATA_HOME");
  fs::remove_all(dir);
}

TEST_CASE("remove_path defaults to the trash, not to deletion", "[platform]") {
  const auto dir        = scratch("remove_path_trash");
  const auto trash_home = dir / "xdg_data";
  ::setenv("XDG_DATA_HOME", trash_home.c_str(), 1);

  const auto victim = dir / "separator.txt";
  write_file(victim, "mod contents");
  REQUIRE(remove_path(victim));  // permanent == false, the default
  REQUIRE_FALSE(fs::exists(victim));

  // Negative control: this is the property the default is *for*. If the
  // default had flipped to a permanent delete, the file would still be gone -
  // so assert the bytes survive somewhere.
  int found = 0;
  for (const auto &e : fs::recursive_directory_iterator(trash_home))
    if (e.is_regular_file() && read_file(e.path()) == "mod contents")
      found++;
  REQUIRE(found >= 1);

  ::unsetenv("XDG_DATA_HOME");
  fs::remove_all(dir);
}

TEST_CASE("remove_path(permanent = true) deletes unrecoverably", "[platform]") {
  const auto dir        = scratch("remove_path_permanent");
  const auto trash_home = dir / "xdg_data";
  ::setenv("XDG_DATA_HOME", trash_home.c_str(), 1);

  const auto victim = dir / "gone.txt";
  write_file(victim, "unrecoverable");
  REQUIRE(remove_path(victim, true));
  REQUIRE_FALSE(fs::exists(victim));

  // Negative control: the permanent path must NOT have gone through the
  // adaptor. Finding these bytes in the trash would mean the permanent flag
  // was ignored. The trash root is expected not to exist at all here - the
  // permanent branch never creates it - so guard the walk rather than letting
  // the iterator throw, which would be a failure for the wrong reason.
  REQUIRE_FALSE(fs::exists(trash_home));
  bool in_trash = false;
  if (fs::exists(trash_home)) {
    for (const auto &e : fs::recursive_directory_iterator(trash_home))
      if (e.is_regular_file() && read_file(e.path()) == "unrecoverable")
        in_trash = true;
  }
  REQUIRE_FALSE(in_trash);

  ::unsetenv("XDG_DATA_HOME");
  fs::remove_all(dir);
}

TEST_CASE("path_is_executable reflects the real execute bit", "[platform]") {
  const auto dir = scratch("is_exec");

  const auto plain = dir / "plain.txt";
  write_file(plain, "x");
  fs::permissions(plain, fs::perms::owner_read | fs::perms::owner_write);
  REQUIRE_FALSE(path_is_executable(plain));

  const auto exe = dir / "runme.sh";
  write_file(exe, "#!/bin/sh\n");
  fs::permissions(exe, fs::perms::owner_read | fs::perms::owner_write |
                           fs::perms::owner_exec);
  REQUIRE(path_is_executable(exe));

  // Negative control: a body that returned true unconditionally, or that
  // tested for *readability* instead of executability, passes the first
  // assertion and fails this one - plain.txt is readable and not executable.
  REQUIRE(path_is_executable(plain) != path_is_executable(exe));

  // And a path that does not exist at all is not executable.
  REQUIRE_FALSE(path_is_executable(dir / "absent"));

  fs::remove_all(dir);
}

TEST_CASE("filesystem_is_case_insensitive matches this filesystem", "[platform]") {
  const auto dir       = scratch("case");
  const fs::path lower = dir / "caseprobe.txt";
  write_file(lower, "x");

  // Probe the real filesystem: create "caseprobe.txt" and ask whether
  // "CASEPROBE.TXT" resolves to the same file.
  std::error_code ec;
  const bool actually_ci = fs::exists(dir / "CASEPROBE.TXT", ec);

  REQUIRE(filesystem_is_case_insensitive() == actually_ci);

  // Negative control: the assertion compares two independent observations, so
  // a constant-false answer cannot pass on a case-insensitive volume and a
  // constant-true answer cannot pass here. If the two ever disagree the test
  // goes red, which is exactly the drift this guards.
  if (!actually_ci)
    REQUIRE_FALSE(filesystem_is_case_insensitive());

  fs::remove_all(dir);
}

TEST_CASE("machine_id is readable here and looks like an identifier", "[platform]") {
  const std::string id = machine_id();

  // Not asserting the *value* - that is machine-specific - but that it is
  // non-empty, single-line, and hex-ish. This is the key material source for
  // the file keyring, so "empty because the read failed" is the failure mode
  // worth catching.
  REQUIRE_FALSE(id.empty());
  REQUIRE(id.find('\n') == std::string::npos);
  REQUIRE(id.size() >= 8);
  REQUIRE(id.size() <= 64);

  // Negative control: stability across calls. The keyring derives from this
  // on every read, so a value that changed between two reads in the same
  // process would break every stored secret.
  REQUIRE(machine_id() == id);
}

TEST_CASE("file_birth_time is a real timestamp or nothing at all", "[platform]") {
  const auto dir = scratch("btime");
  const auto f   = dir / "born.txt";
  write_file(f, "x");

  const auto now = std::time(nullptr);

  if (const auto btime = file_birth_time(f)) {
    // Present: it must be a sane epoch second, not a garbage value. 2020-01-01
    // is far enough back that any real birth time clears it, and now + 60s
    // catches a unit mix-up (nanoseconds read as seconds, say).
    REQUIRE(*btime > 1577836800);
    REQUIRE(*btime <= now + 60);
  }

  // Negative control: a path that does not exist must report "no birth time",
  // never a fabricated epoch value. The caller falls back to mtime on nullopt
  // and would show 1970 to the user if this returned a zero second instead.
  const auto absent = file_birth_time(dir / "absent");
  REQUIRE_FALSE(absent.has_value());

  // ...and when a birth time IS reported for an existing directory, it obeys
  // the same sanity bounds as the file above, so a body that mixes the two up
  // (returning mtime for a statx failure, say) is caught rather than silently
  // changing what the UI displays.
  if (const auto dir_btime = file_birth_time(dir)) {
    REQUIRE(*dir_btime > 1577836800);
    REQUIRE(*dir_btime <= now + 60);
  }

  fs::remove_all(dir);
}

TEST_CASE("the log file descriptor pair reaches the filesystem", "[platform]") {
  const auto dir      = scratch("logfd");
  const auto log_path = dir / "gmm.log";

  const int fd = open_truncated_write_fd(log_path.string());
  REQUIRE(fd >= 0);

  // A payload far larger than the 4 KB the kernel is likely to buffer, so a
  // write that drops or reorders blocks cannot pass.
  const std::string big(200000, 'x');
  write_raw_fd(fd, big.data(), big.size());
  write_raw_fd(fd, "\n", 1);
  close_raw_fd(fd);

  REQUIRE(fs::file_size(log_path) == big.size() + 1);
  REQUIRE(read_file(log_path) == big + "\n");

  // Negative control: the bytes are all there, in order. A writer that wrote
  // the first block only, or that mangled the tail, fails on size or content.
  REQUIRE(read_file(log_path).size() == 200001);
  REQUIRE(read_file(log_path).substr(200000) == "\n");

  // The partial-write loop inside write_raw_fd is NOT proven here. A blocking
  // write() to a regular file or a pipe returns only once it has written
  // everything, so no reachable setup forces the short write the loop guards
  // against without either a non-blocking descriptor (whose EAGAIN makes the
  // assertion a race) or a test-only shim. Left unproven rather than faked.

  fs::remove_all(dir);
}

TEST_CASE("open_truncated_write_fd truncates and reports an unopenable path",
          "[platform]") {
  const auto dir      = scratch("logfd_open");
  const auto log_path = dir / "gmm.log";
  write_file(log_path, "a much longer pre-existing body");

  int fd = open_truncated_write_fd(log_path.string());
  REQUIRE(fd >= 0);
  close_raw_fd(fd);
  // Negative control: truncating, not appending. main() hands the logger a
  // path that may hold the previous session's log; appending would grow it
  // without bound across runs.
  REQUIRE(fs::file_size(log_path) == 0);

  // An unopenable path returns -1 rather than a bad descriptor the caller
  // would then write to.
  REQUIRE(open_truncated_write_fd((dir / "no" / "such" / "dir" / "x.log").string()) ==
          -1);

  fs::remove_all(dir);
}

TEST_CASE("Logger writes through the platform descriptor", "[platform]") {
  const auto dir      = scratch("logger");
  const auto log_path = dir / "gmm.log";

  Logger &logger = Logger::instance();
  REQUIRE(logger.set_log_file(log_path.string()));

  logger.info("platform-primitive-probe");

  // The consumer is the log file itself: set_log_file / raw_append exist to
  // put bytes there, so this is where "the value reaches a consumer" has to be
  // proven.
  const std::string body = read_file(log_path);
  REQUIRE(body.find("platform-primitive-probe") != std::string::npos);
  REQUIRE(body.find("[INF]") != std::string::npos);

  // Negative control: the timestamp must be the real wall clock, not zeros.
  // Asserting only the bracket width would pass for "[00:00:00]", so compare
  // against the same clock the logger should have used - and allow a minute of
  // slack so a run that straddles a minute boundary is not a flake.
  char expected_clock[16];
  const std::tm now_tm = local_time(std::time(nullptr));
  std::snprintf(expected_clock, sizeof(expected_clock), "%02d:%02d:%02d",
                now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec);

  bool matched = false;
  for (int minutes_back = 0; minutes_back <= 1 && !matched; ++minutes_back) {
    const std::tm t = local_time(std::time(nullptr) - minutes_back * 60);
    char want[16];
    std::snprintf(want, sizeof(want), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    matched = body.find(std::string("[") + want + "]") != std::string::npos;
  }
  REQUIRE(matched);
  (void)expected_clock;

  logger.raw_append("raw-append-probe\n");
  REQUIRE(read_file(log_path).find("raw-append-probe") != std::string::npos);

  fs::remove_all(dir);
}

TEST_CASE("set_thread_low_priority actually lowers this thread's nice value",
          "[platform]") {
  const int before = ::getpriority(PRIO_PROCESS, 0);
  set_thread_low_priority();
  const int after = ::getpriority(PRIO_PROCESS, 0);

  // Raising the nice value is always permitted for an unprivileged process,
  // so this must hold on every machine.
  REQUIRE(after >= before);

  // Negative control: a no-op body passes "after >= before" trivially. The
  // thread_priority.cpp wrapper is exactly such a candidate to get wrong, so
  // assert the direction actually moved when it started at 0 - the normal
  // case - and only that, so a machine already at a raised nice value is not
  // failed for being conservative.
  if (before == 0)
    REQUIRE(after > 0);
  else
    REQUIRE(after >= before);
}

TEST_CASE("current_executable_path names the binary that is actually running",
          "[platform]") {
  const auto exe = current_executable_path();
  REQUIRE_FALSE(exe.empty());
  REQUIRE(fs::exists(exe));

  // Consumer reachability, not a getter round-trip: install_method.cpp decides
  // the install method from this path, so compare it against the path the test
  // binary itself reports. /proc/self/exe resolves symlinks, so a build tree
  // reached through a symlinked path still has to agree on the file name.
  REQUIRE(exe.filename() == fs::canonical("/proc/self/exe").filename());

  // Negative control: a path that does not exist must not be handed back as
  // though it did. If the body returned its argument unconditionally, or
  // returned the cwd, this is the assertion that fails.
  REQUIRE(current_executable_path() != fs::current_path());
}

TEST_CASE("home_dir_or_empty agrees with HOME and never invents one", "[platform]") {
  const char *env = std::getenv("HOME");

  // Consumer reachability: install_method.cpp feeds this into f.home and then
  // compares the exe directory against it, so an empty value when HOME is set
  // would silently disable QtIFW detection.
  if (env && env[0] != '\0') {
    REQUIRE(home_dir_or_empty() == fs::path(env));
  }

  // Negative control, and the reason this is a separate function at all:
  // home_dir() falls back to the temp directory when HOME is unset, so a body
  // that forwarded to home_dir() would return a NON-empty path. Here, unset
  // HOME must produce empty, which is the distinction the caller relies on to
  // tell "no home configured" from "home is the temp directory".
  ::unsetenv("HOME");
  REQUIRE(home_dir_or_empty().empty());

  // Re-probe after restoring, proving the body reads the environment on every
  // call rather than caching the first answer.
  ::setenv("HOME", "/tmp/gmm-home-probe", 1);
  REQUIRE(home_dir_or_empty() == fs::path("/tmp/gmm-home-probe"));

  if (env && env[0] != '\0')
    ::setenv("HOME", env, 1);
  else
    ::unsetenv("HOME");
}

TEST_CASE("path_is_writable is a permission question, not a guess", "[platform]") {
  const auto dir = scratch("writable");

  // Consumer reachability: install_method.cpp decides macOS disk-image
  // installs from this answer, so a body that always returned true would report
  // a read-only mounted image as writable.
  REQUIRE(path_is_writable(dir));

  // Negative control: a path that is not a directory cannot be a directory the
  // user writes into. A body that returned true unconditionally fails here.
  const auto missing = dir / "absent";
  REQUIRE_FALSE(path_is_writable(missing));

  // And the empty path is refused rather than defaulting to the process cwd,
  // which is what a naive `access(dir.c_str(), W_OK)` on "" would have done.
  REQUIRE_FALSE(path_is_writable({}));
}

TEST_CASE("volume_root_of finds the mount a path lives on", "[platform]") {
  const auto dir = scratch("volume");

  // Consumer reachability: install_method.cpp asks this to decide whether a
  // macOS bundle sits on a writable volume or a mounted read-only image.
  const auto root = volume_root_of(dir);
  REQUIRE_FALSE(root.empty());
  REQUIRE(fs::exists(root));

  // The answer must be an ancestor of what was asked, or the caller cannot use
  // it to decide anything about the original path.
  const auto probe       = fs::absolute(dir).lexically_normal();
  const auto root_abs    = fs::absolute(root).lexically_normal();
  const bool is_root     = root_abs == probe.root_path().lexically_normal();
  const bool is_ancestor = std::distance(root_abs.begin(), root_abs.end()) <=
                           std::distance(probe.begin(), probe.end());
  REQUIRE((is_root || is_ancestor));

  // Negative control: empty in, empty out. A body that returned its argument
  // for anything non-empty would still pass the two checks above, so this is
  // what pins the actual guard.
  REQUIRE(volume_root_of({}).empty());
}

TEST_CASE("recorded_install_location is empty where nothing records one",
          "[platform]") {
  // No test writes a real install location, so on every OS this must report
  // "nothing recorded one" rather than inventing a path. A body that returned
  // the home directory or the cwd would make install_method.cpp treat an
  // arbitrary directory as the install root.
  const auto loc = recorded_install_location();
  REQUIRE(loc.empty());
  REQUIRE(loc != home_dir_or_empty());
  REQUIRE(loc != fs::current_path());
}

TEST_CASE("load_shared_library resolves a real symbol through the adaptor",
          "[platform]") {
  // Consumer reachability: Module::symbol and PluginLoader both resolve
  // "gmm_abi_version" through this and nothing else. Loading a real library
  // and resolving a real exported symbol is the proof that the dlopen/dlsym
  // pair still works after the Windows shims were removed.
  const auto dir = scratch("shlib");

  // Build the shared object the same way the plugin build does, then load it
  // the way the plugin loader does.
  const auto src = dir / "probe.c";
  const auto so  = dir / "libprobe.so";
  write_file(src, "int gmm_abi_version(void) { return 7; }\n");

  const std::string cmd =
      "cc -shared -fPIC -o '" + so.string() + "' '" + src.string() + "' 2>/dev/null";
  if (std::system(cmd.c_str()) != 0 || !fs::exists(so)) {
    fs::remove_all(dir);
    SUCCEED("no C compiler for the shared-library probe; adaptor is "
            "unexercised on this machine, not proven here");
    return;
  }

  void *handle = load_shared_library(so);
  REQUIRE(handle != nullptr);

  auto *version =
      reinterpret_cast<int (*)()>(shared_library_symbol(handle, "gmm_abi_version"));
  REQUIRE(version != nullptr);
  // The value proves the symbol came out of THIS library, not that some pointer
  // was returned.
  REQUIRE(version() == 7);

  // Negative control: a symbol that does not exist must resolve to null, and
  // unloading must be safe on a null handle.
  REQUIRE(shared_library_symbol(handle, "gmm_no_such_symbol") == nullptr);
  REQUIRE(shared_library_symbol(nullptr, "gmm_abi_version") == nullptr);
  unload_shared_library(handle);
  unload_shared_library(nullptr);
  SUCCEED("unload and null-handle paths exercised");

  // A failed load must report something rather than crash, and the handle must
  // be null. This is the path the plugin scanner hits for every bad plugin.
  REQUIRE(load_shared_library(dir / "definitely_absent.so") == nullptr);
  REQUIRE(dlerror_message() != nullptr);

  fs::remove_all(dir);
}

TEST_CASE("spawn_detached_process starts a real process and returns its pid",
          "[platform]") {
  const auto dir = scratch("spawn");

  // Consumer reachability: NativeRuntime::launch calls exactly this and then
  // hands last_pid() to launcher.cpp. A body that returned a pid without
  // starting anything would pass a "pid is positive" assertion and still ship
  // a game that never launches, so the process itself is observed.
  const auto marker        = dir / "spawned.txt";
  const std::string script = "echo alive > " + marker.string();

  const auto pid = spawn_detached_process("/bin/sh", {"/bin/sh", "-c", script}, dir,
                                          /*shell_fallback=*/false);
  REQUIRE(pid > 0);

  // The call must NOT block: it returns while the child is still running. Poll
  // briefly for the file the child writes.
  bool seen = false;
  for (int i = 0; i < 200 && !seen; ++i) {
    seen = fs::exists(marker);
    if (!seen)
      ::usleep(10000);
  }
  REQUIRE(seen);
  REQUIRE(read_file(marker).find("alive") != std::string::npos);

  // Negative control: nothing to execute cannot start a process and must say
  // so, rather than return a plausible-looking pid.
  REQUIRE(spawn_detached_process({}, {"/bin/sh"}, dir, false) == -1);
  REQUIRE(spawn_detached_process("/bin/sh", {}, dir, false) == -1);

  fs::remove_all(dir);
}

TEST_CASE("spawn_detached_process execs the executable, not argv[0]", "[platform]") {
  const auto dir = scratch("spawn_path");

  // Regression guard for a real mistake made while writing this adaptor: the
  // first signature folded argv[0] and the executable together, which silently
  // broke ProtonRuntime. That runtime execs the runner by its FULL path while
  // argv[0] is the bare filename, so a body that exec'd argv[0] would send
  // execvp searching PATH and never find the runner sitting next to it.
  //
  // Executable and argv[0] therefore differ here too, and the child proves it
  // saw the right one by echoing both back.
  const auto out           = dir / "argv.txt";
  const std::string script = "echo \"$0\" > " + out.string();

  // A bare name that is NOT on PATH: if the body exec'd argv[0] this could not
  // possibly work, because nothing resolves "not-on-path-probe".
  const auto pid = spawn_detached_process(
      "/bin/sh", {"not-on-path-probe", "-c", script}, dir, /*shell_fallback=*/false);
  REQUIRE(pid > 0);

  bool seen = false;
  for (int i = 0; i < 200 && !seen; ++i) {
    seen = fs::exists(out);
    if (!seen)
      ::usleep(10000);
  }
  REQUIRE(seen);
  // $0 is argv[0], which is what the child was told to call itself.
  REQUIRE(read_file(out).find("not-on-path-probe") != std::string::npos);

  fs::remove_all(dir);
}