#include "engine/core/log/crash_handler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

// POSIX-only system headers. These must be included at global scope so the
// C library symbols (open/write/close, signal, backtrace, ...) land in the
// global namespace, not inside `namespace engine`.
#ifndef _WIN32
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "platform/platform.h"

namespace engine {

std::string CrashHandler::dump_dir_;

// ---------------------------------------------------------------------------
// Default dump directory
//
// Mirrors Platform::cache_dir() / "crash_dumps" on every platform so
// crash dumps land next to the rest of the app's cached data. Falls back to
// safe_home_dir() when the platform-specific environment variable is unset.
// ---------------------------------------------------------------------------
std::string CrashHandler::default_dump_dir() {
#ifdef _WIN32
  std::filesystem::path base;
  if (const wchar_t *la = _wgetenv(L"LOCALAPPDATA"); la && la[0] != L'\0') {
    base = la;
  } else {
    base = safe_home_dir() / L"AppData" / L"Local";
  }
  return (base / L"gamemodmanager" / L"cache" / L"crash_dumps").string();
#elif defined(__APPLE__)
  return (safe_home_dir() / "Library" / "Caches" / "GameModManager" / "crash_dumps")
      .string();
#else
  std::filesystem::path base;
  if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0] != '\0') {
    base = xdg;
  } else {
    base = safe_home_dir() / ".cache";
  }
  return (base / "GameModManager" / "crash_dumps").string();
#endif
}

// ---------------------------------------------------------------------------
// Prune old crash dumps at startup
// ---------------------------------------------------------------------------

void CrashHandler::prune_old_dumps(int max_kept, const std::string &dump_dir) {
  if (max_kept <= 0)
    return;  // 0 = disable pruning entirely

  const std::string dir = dump_dir.empty() ? dump_dir_ : dump_dir;
  if (dir.empty())
    return;

  std::error_code ec;
  std::filesystem::path dir_path(dir);
  if (!std::filesystem::is_directory(dir_path, ec))
    return;

  // Collect all .dmp files in the directory
  std::vector<std::filesystem::directory_entry> dumps;
  for (const auto &entry : std::filesystem::directory_iterator(dir_path, ec)) {
    if (!entry.is_regular_file())
      continue;
    auto ext = entry.path().extension().string();
    // Case-insensitive extension match
    for (auto &c : ext)
      c = static_cast<char>(std::tolower(c));
    if (ext == ".dmp")
      dumps.push_back(entry);
  }

  if (static_cast<int>(dumps.size()) <= max_kept)
    return;

  // Sort newest-first by last write time
  std::sort(dumps.begin(), dumps.end(),
            [&ec](const std::filesystem::directory_entry &a,
                  const std::filesystem::directory_entry &b) {
              return a.last_write_time(ec) > b.last_write_time(ec);
            });

  // Delete oldest entries beyond the limit
  for (auto it = dumps.begin() + max_kept; it != dumps.end(); ++it) {
    std::filesystem::remove(it->path(), ec);
  }
}

// ---------------------------------------------------------------------------
// Windows: MiniDumpWriteDump via the unhandled-exception filter
// ---------------------------------------------------------------------------
#ifdef _WIN32

namespace {

  std::filesystem::path g_dump_path;

  LONG WINAPI windows_exception_handler(EXCEPTION_POINTERS *exception_info) {
    HANDLE hFile = CreateFileW(g_dump_path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION info{};
      info.ThreadId          = GetCurrentThreadId();
      info.ExceptionPointers = exception_info;
      info.ClientPointers    = FALSE;
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                        MiniDumpNormal, &info, nullptr, nullptr);
      CloseHandle(hFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
  }

  std::filesystem::path make_dump_path(const std::string &dir) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char filename[128];
    std::snprintf(filename, sizeof(filename), "%04d.%02d.%02d-%02d.%02d.%02d.dmp",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::filesystem::path(dir) / filename;
  }

}  // namespace

void CrashHandler::install(const std::string &dump_dir) {
  dump_dir_ = dump_dir;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dump_dir), ec);
  g_dump_path = make_dump_path(dump_dir);
  SetUnhandledExceptionFilter(windows_exception_handler);
}

void CrashHandler::uninstall() {
  SetUnhandledExceptionFilter(nullptr);
}

// ---------------------------------------------------------------------------
// POSIX (Linux + macOS): signal handler + backtrace
// ---------------------------------------------------------------------------
#else

static constexpr int kMaxFrames = 64;

// The alternate signal stack, and the reason it exists: a handler that runs on
// the thread stack that just overflowed cannot push its own frame. The first
// write lands past the guard page, the kernel blocks SIGSEGV for the duration
// of the handler, so the nested fault takes the default action and the process
// dies with no dump at all - which is exactly the bare exit 139 the crash
// reports carried. sigaltstack() plus SA_ONSTACK puts the handler here, where
// it has room to walk the very stack it was interrupted on. MINSIGSTKSZ (2 KiB)
// is far too small for backtrace(), hence a fixed 64 KiB.
//
// One buffer serves every thread on purpose: sigaction is process-wide, so any
// thread may fault, but the handler ends in _exit() and never returns, so at
// most one thread is ever inside it. Per-thread stacks would cost 64 KiB of TLS
// on every thread in the process to close a window that only opens in the
// microseconds before the process dies anyway.
static char g_alt_stack[64 * 1024];

// --- Async-signal-safe primitives ------------------------------------------
// The handler interrupts an arbitrary instruction, which may be inside malloc
// or inside a libc lock. Nothing below allocates, takes a lock, or touches
// std::string: only open/read/write/close over fixed buffers.

static void sa_write(int fd, const char *s, size_t n) {
  size_t off = 0;
  while (off < n) {
    const auto w = ::write(fd, s + off, n - off);
    if (w <= 0)
      return;
    off += static_cast<size_t>(w);
  }
}

static void sa_puts(int fd, const char *s) {
  sa_write(fd, s, std::strlen(s));
}

static void sa_put_hex_line(int fd, const char *label, uintptr_t value) {
  char buf[96];
  const int n = std::snprintf(buf, sizeof(buf), "%s0x%zx\n", label, value);
  if (n > 0)
    sa_write(fd, buf, static_cast<size_t>(n));
}

// The interrupted program counter, read straight out of the context the kernel
// handed us. This is the faulting instruction, obtained without unwinding
// anything, so it survives every case where backtrace() cannot describe the
// interrupted frame: an exhausted stack, a frame inside a library built without
// unwind tables, or a JIT frame carrying no .eh_frame at all.
static uintptr_t fault_pc(const void *ctx) {
  if (ctx == nullptr)
    return 0;
#if defined(__APPLE__) && defined(__x86_64__)
  const auto *uc = static_cast<const ucontext_t *>(ctx);
  return static_cast<uintptr_t>(uc->uc_mcontext->__ss.__rip);
#elif defined(__APPLE__) && defined(__aarch64__)
  const auto *uc = static_cast<const ucontext_t *>(ctx);
  return static_cast<uintptr_t>(uc->uc_mcontext->__ss.__pc);
#elif defined(__linux__) && defined(__x86_64__)
  const auto *uc = static_cast<const ucontext_t *>(ctx);
  return static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
  const auto *uc = static_cast<const ucontext_t *>(ctx);
  return static_cast<uintptr_t>(uc->uc_mcontext.pc);
#else
  return 0;
#endif
}

// /proc/self/maps, so every bare address in this dump - the faulting PC, the
// frames below, and anything inside a library that carries no symbol table -
// resolves to a module plus offset offline. Copied through a fixed buffer
// rather than getline(), which allocates.
static void write_maps(int fd) {
  const int mfd = ::open("/proc/self/maps", O_RDONLY);
  if (mfd < 0)
    return;
  char buf[8192];
  for (;;) {
    const auto n = ::read(mfd, buf, sizeof(buf));
    if (n <= 0)
      break;
    sa_write(fd, buf, static_cast<size_t>(n));
  }
  ::close(mfd);
}

// Recursively create directories (async-signal-safe subset: only uses mkdir + stat).
static bool mkdirs(const std::string &path, mode_t mode) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    return true;

  // Find the parent
  size_t pos = path.find_last_of('/');
  if (pos != std::string::npos && pos > 0) {
    std::string parent = path.substr(0, pos);
    if (!parent.empty())
      mkdirs(parent, mode);
  }

  return mkdir(path.c_str(), mode) == 0 || errno == EEXIST;
}

void CrashHandler::write_dump(int sig, const siginfo_t *info, const void *ctx) {
  using namespace std::chrono;
  auto now        = system_clock::now();
  auto time_t_now = system_clock::to_time_t(now);

  struct tm tm_buf{};
  localtime_r(&time_t_now, &tm_buf);

  char filename[128];
  std::snprintf(filename, sizeof(filename), "%s/%04d.%02d.%02d-%02d.%02d.%02d.dmp",
                dump_dir_.c_str(), tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

  int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;

  const char *sig_name = "UNKNOWN";
  switch (sig) {
  case SIGSEGV:
    sig_name = "SIGSEGV (segmentation fault)";
    break;
  case SIGABRT:
    sig_name = "SIGABRT (abort)";
    break;
  case SIGFPE:
    sig_name = "SIGFPE (floating point exception)";
    break;
  case SIGBUS:
    sig_name = "SIGBUS (bus error)";
    break;
  case SIGILL:
    sig_name = "SIGILL (illegal instruction)";
    break;
  }

  sa_puts(fd, "=== GameModManager Crash Dump ===\n");
  sa_puts(fd, "Signal: ");
  sa_puts(fd, sig_name);
  sa_puts(fd, "\n");

  char ts_buf[64];
  std::snprintf(ts_buf, sizeof(ts_buf), "Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  sa_puts(fd, ts_buf);

  // si_addr is defined for the fault signals only; for SIGABRT the union
  // member at that offset means something else entirely.
  const bool is_fault_signal =
      sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE;
  if (is_fault_signal && info != nullptr) {
    sa_put_hex_line(fd, "Fault address: ", reinterpret_cast<uintptr_t>(info->si_addr));
  }
  const uintptr_t pc = fault_pc(ctx);
  if (pc != 0)
    sa_put_hex_line(fd, "Faulting PC: ", pc);

  // Name the faulting frame from its own address, before and independently of
  // the walk below: backtrace_symbols() over a single frame needs no
  // unwinder, so the function that actually crashed is in the dump even in the
  // cases where the walk returns nothing past the signal trampoline.
  if (pc != 0) {
    void *one[1] = {reinterpret_cast<void *>(pc)};
    char **sym   = backtrace_symbols(one, 1);
    if (sym != nullptr) {
      sa_puts(fd, "Faulting frame: ");
      sa_puts(fd, sym[0]);
      sa_puts(fd, "\n");
    }
  }

  sa_puts(fd, "=== Stack Trace ===\n");

  void *frames[kMaxFrames];
  const int count = backtrace(frames, kMaxFrames);
  char **symbols  = backtrace_symbols(frames, count);

  if (symbols != nullptr) {
    for (int i = 0; i < count; ++i) {
      sa_puts(fd, symbols[i]);
      sa_puts(fd, "\n");
    }
    // No free(): the array is gone with the process two lines after this, and
    // calling into the allocator from a handler that may have interrupted the
    // allocator is the one thing here that cannot be made safe.
  } else {
    sa_puts(fd, "(backtrace_symbols unavailable)\n");
  }

  // The unwinder stops at the first frame it cannot describe, and says
  // nothing when it does. Without this line a three-frame trace reads exactly
  // like a complete one, which is how four unreproducible dumps came to be
  // read as "the crash site is unknown" when in fact the trace had simply
  // dead-ended before reaching it.
  sa_puts(fd, "=== Unwind Ends Here ===\n");
  sa_puts(fd, "backtrace() returned the frames above and stopped. Anything below "
              "this line was not recovered: trust 'Faulting PC' and 'Faulting "
              "frame' over the tail of the trace.\n");

  sa_puts(fd, "=== Memory Maps ===\n");
  write_maps(fd);

  sa_puts(fd, "=== End Dump ===\n");
  ::close(fd);
}

void CrashHandler::signal_handler(int sig, siginfo_t *info, void *ctx) {
  write_dump(sig, info, ctx);
  _exit(128 + sig);
}

void CrashHandler::install(const std::string &dump_dir) {
  dump_dir_ = dump_dir;
  mkdirs(dump_dir_, 0755);

  stack_t alt{};
  alt.ss_sp    = g_alt_stack;
  alt.ss_size  = sizeof(g_alt_stack);
  alt.ss_flags = 0;
  ::sigaltstack(&alt, nullptr);

  struct sigaction sa{};
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = signal_handler;
  // SA_SIGINFO: the siginfo and ucontext carry the faulting address and PC.
  // SA_ONSTACK: run on g_alt_stack, so an overflowed thread stack does not
  // kill the handler before it can report anything.
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
}

void CrashHandler::uninstall() {
  signal(SIGSEGV, SIG_DFL);
  signal(SIGABRT, SIG_DFL);
  signal(SIGFPE, SIG_DFL);
  signal(SIGBUS, SIG_DFL);
  signal(SIGILL, SIG_DFL);
}

#endif  // _WIN32

// Free-function entry point used by main().
void install_crash_handler() {
  CrashHandler::install(CrashHandler::default_dump_dir());
}

}  // namespace engine
