#pragma once

// Lightweight parallel-for facade for the engine. Engine is Qt-free, so this
// uses std::thread (mirroring the proven run_parallel helper in
// deploy_utils.cpp:94). When `enabled()` is false or the work is too small,
// for_each runs sequentially. The Settings bridge lives in
// settings_content_widget.cpp (the "Enable multi-core processing" checkbox
// wires Settings::performance_multi_core() through parallel::set_enabled on
// toggle).
//
// Threading contract: `fn` is invoked on worker threads concurrently. The
// caller is responsible for any shared-state synchronization. Per-index
// dispatch (atomic counter) means the index a worker sees is NOT sequential -
// collect results via a per-index slot, mutex, or pre-sized vector.
//
// Note: Deploy (deploy_utils.cpp:run_parallel) intentionally does NOT route
// through this facade - the user directive keeps Deploy always-parallel with
// hardware_concurrency capped at 16, independent of the toggle.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace engine::parallel {

// Multi-core toggle. Defaults to ON so the app benefits on first launch
// without the user having to open Settings; the Settings checkbox pushes the
// persisted value through set_enabled() on change.
inline std::atomic<bool> &enabled_storage() {
  static std::atomic<bool> e{true};
  return e;
}

inline bool enabled() {
  return enabled_storage().load(std::memory_order_relaxed);
}

inline void set_enabled(bool on) {
  enabled_storage().store(on, std::memory_order_relaxed);
}

// 0 = hardware_concurrency capped at 16 (matches deploy_utils.cpp:94 cap).
// Returns 1 when disabled so a small n never spawns a thread per item.
inline unsigned thread_count(size_t n) {
  if (!enabled() || n < 2)
    return 1;
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0)
    hc = 1;
  unsigned t = std::min(hc, 16u);
  t = std::min<unsigned>(t, static_cast<unsigned>(n));
  return std::max(t, 1u);
}

// Run fn(i) for i in [0, n) concurrently across `thread_count(n)` worker
// threads when enabled and n is large enough to be worth it; otherwise
// sequential. fn is invoked with no synchronization - it MUST NOT mutate
// shared state without external sync, or use a per-index slot/mutex.
template <typename Fn> inline void for_each(size_t n, Fn &&fn) {
  if (n == 0)
    return;
  const unsigned t = thread_count(n);
  if (t <= 1) {
    for (size_t i = 0; i < n; ++i)
      fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(t);
  for (unsigned k = 0; k < t; ++k) {
    pool.emplace_back([&fn, &next, n]() {
      for (;;) {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= n)
          break;
        fn(i);
      }
    });
  }
  for (auto &th : pool)
    th.join();
}

} // namespace engine::parallel