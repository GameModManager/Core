#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace engine::profile {

// Debounced background file writer (MO2's DelayedFileWriter pattern, Qt-free).
//
// The engine is Qt-free, so the ~5s debounce MO2 implements with a QTimer is
// implemented here with a dedicated writer thread + condition variable:
//
//   - write()            schedules a write ~delay after the LAST call
//                        (calling write() again restarts the delay)
//   - write_immediately() forces a flush of any pending write now
//   - cancel()           discards a pending write (no-op if none pending)
//   - ~DelayedFileWriter flushes anything still pending (changes are never
//                        silently lost)
//
// The callback runs on the writer thread; it must be thread-safe with respect
// to the state it reads (the Profile serializes its mod list with a mutex).
// Exceptions thrown by the callback are caught and logged so the writer
// thread never dies via std::terminate.
class DelayedFileWriter {
public:
    using WriteFn = std::function<void()>;

    explicit DelayedFileWriter(WriteFn fn,
                               std::chrono::milliseconds delay = std::chrono::seconds(5));
    ~DelayedFileWriter();

    DelayedFileWriter(const DelayedFileWriter&) = delete;
    DelayedFileWriter& operator=(const DelayedFileWriter&) = delete;
    DelayedFileWriter(DelayedFileWriter&&) = delete;
    DelayedFileWriter& operator=(DelayedFileWriter&&) = delete;

    // Schedule a write ~delay from now. Debounced: repeated calls within the
    // delay window collapse into a single write.
    void write();

    // Flush a pending write now, synchronously: when this returns the write
    // has completed. No-op when nothing is pending.
    void write_immediately();

    // Discard a pending write. No-op when nothing is pending.
    void cancel();

private:
    void run();

    WriteFn fn_;
    std::chrono::milliseconds delay_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = false;    // a write is scheduled
    bool stop_ = false;       // thread shutdown requested
    uint64_t generation_ = 0; // bumped on every write() to restart the delay
    std::thread thread_;
};

}  // namespace engine::profile