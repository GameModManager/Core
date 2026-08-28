#pragma once

namespace engine {

// Best-effort: lower the priority of the calling thread/process so heavy,
// CPU-bound work (archive extraction) yields to the rest of the system and
// keeps it responsive. Failures are ignored — this is never fatal.
//
// - Windows: adjusts the current thread only (SetThreadPriority).
// - Linux/macOS (POSIX): adjusts the current process's nice value
//   (10 = below normal). This is inherited by the worker thread that runs the
//   extraction and by any child processes it spawns (e.g. the unrar CLI
//   fallback), and by the main thread — but the main thread is idle while
//   extraction runs, so the whole app simply yields to other processes, which
//   is exactly the desired "keep the system responsive" behavior.
void set_low_priority() noexcept;

}  // namespace engine
