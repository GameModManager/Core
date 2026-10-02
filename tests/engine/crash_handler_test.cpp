// crash_handler_test.cpp - the crash dump has to name the function that
// crashed, or every crash in this project is guesswork.
//
// The bug this guards: the handler ran on whatever thread stack it was
// interrupted on and walked it with backtrace(). A crash that exhausts the
// stack therefore killed the handler before the handler could report anything -
// the first nested write landed past the guard page, the kernel blocks SIGSEGV
// while the handler runs, so the nested fault took the default action and the
// process died with no dump at all, just a bare exit 139. That is the shape
// every dump from the 3-frame-trace investigation had.
//
// The fix moves the handler onto a sigaltstack and reads the interrupted
// program counter straight out of the ucontext, so the faulting frame is
// reported from the kernel's own record of it rather than from a walk that
// may dead-end before reaching it.
//
// Each crash runs in a forked child: the handler ends in _exit(), so a crash
// in the test process itself would take Catch2 down with it and report
// nothing. The child installs the handler into a throwaway directory and
// crashes; the parent reads the dump back off disk and asserts on it.
#include "engine/core/log/crash_handler.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {

std::filesystem::path g_root;

void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

std::string read_dump(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::path only;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".dmp") {
      only = entry.path();
      break;
    }
  }
  if (only.empty())
    return {};
  std::ifstream in(only);
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// Fork, crash the child, and hand back the dump it wrote. Returns an empty
// string when the child produced no dump at all - the pre-fix failure mode.
// Takes int (*)() because the stack-exhaustion case has to defeat tail-call
// optimisation, which means consuming its own return value.
std::string crash_in_child(int (*crash)()) {
  const std::filesystem::path dir = g_root / "crash_dumps";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  const pid_t pid = fork();
  if (pid < 0)
    return {};
  if (pid == 0) {
    engine::CrashHandler::install(dir.string());
    crash();
    // Only reached if the deliberate crash did not crash.
    _exit(0);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return {};
  // The handler exits 128+signal, so SIGSEGV lands on 139.
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 128 + SIGSEGV)
    return {};
  return read_dump(dir);
}

}  // namespace

// The two crash sites deliberately sit OUTSIDE the anonymous namespace, and
// that is load-bearing: backtrace_symbols() resolves names through dladdr,
// which only sees .dynsym, and an internal-linkage function never lands there
// however the executable is linked. With these external, -rdynamic (see
// tests/CMakeLists.txt) puts them in .dynsym and the dump can name them.

// Crashes by dereferencing null. The target goes through a volatile global so
// the compiler cannot prove it is null and delete the store as undefined
// behaviour - which would leave the child exiting cleanly with no crash.
static int *volatile g_null_target = nullptr;

[[gnu::noinline]] void null_deref_crash_site() {
  *g_null_target = 1;
}

// Recurses until the stack is exhausted. The result is consumed AFTER the
// recursive call on purpose: a self-tail-call compiles to a sibcall, which
// pops the frame instead of growing the stack, and this would then spin
// forever at constant stack depth instead of ever overflowing.
[[gnu::noinline]] int stack_overflow_crash_site(int depth) {
  volatile char frame[4096];
  frame[0]        = static_cast<char>(depth);
  const int child = stack_overflow_crash_site(depth + 1);
  frame[1]        = static_cast<char>(child);
  return frame[1];
}

TEST_CASE("crash dump names the crashing function", "[engine]") {
  g_root = std::filesystem::temp_directory_path() / "gmm_crash_handler_test";
  std::filesystem::remove_all(g_root);
  std::filesystem::create_directories(g_root);

  SECTION("null dereference") {
    const std::string dump = crash_in_child([] {
      null_deref_crash_site();
      return 0;
    });
    REQUIRE(!dump.empty());
    INFO("dump:\n" << dump);
    check(dump.find("null_deref_crash_site") != std::string::npos,
          "the dump names the function that crashed");
    check(dump.find("Faulting PC:") != std::string::npos,
          "the dump records the interrupted program counter");
  }

  SECTION("stack exhaustion") {
    // The case the handler could not survive before: it ran on the exhausted
    // stack and faulted again before writing anything, so this section found
    // an empty dump.
    const std::string dump = crash_in_child([] {
      return stack_overflow_crash_site(0);
    });
    REQUIRE(!dump.empty());
    INFO("dump:\n" << dump);
    check(dump.find("stack_overflow_crash_site") != std::string::npos,
          "a stack overflow still names the function that overflowed it");
    check(dump.find("Faulting PC:") != std::string::npos,
          "a stack overflow still records the faulting program counter");
  }
}