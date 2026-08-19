#include "engine/profile/delayed_file_writer.h"

#include "engine/core/log/logger.h"

namespace engine::profile {

DelayedFileWriter::DelayedFileWriter(WriteFn fn, std::chrono::milliseconds delay)
    : fn_(std::move(fn)), delay_(delay), thread_([this] { run(); }) {}

DelayedFileWriter::~DelayedFileWriter() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }

    // The writer thread may have exited without flushing (e.g. it was still
    // inside the debounce delay when shutdown was requested). Flush anything
    // still pending so pending changes are never silently lost.
    bool flush = false;
    {
        std::lock_guard lock(mutex_);
        flush = pending_;
        pending_ = false;
    }
    if (flush) {
        try {
            fn_();
        } catch (const std::exception& e) {
            Logger::instance().error(std::string("DelayedFileWriter flush failed: ") + e.what());
        } catch (...) {
            Logger::instance().error("DelayedFileWriter flush failed: unknown exception");
        }
    }
}

void DelayedFileWriter::write() {
    {
        std::lock_guard lock(mutex_);
        pending_ = true;
        ++generation_;
    }
    cv_.notify_all();
}

void DelayedFileWriter::write_immediately() {
    // Flush synchronously on the caller's thread: when this returns the write
    // has happened (MO2's writeImmediately semantics). The pending flag is
    // cleared under the lock so the writer thread cannot double-flush; if the
    // thread is mid-flush, pending_ is already false and this is a no-op.
    bool do_flush = false;
    {
        std::lock_guard lock(mutex_);
        if (!pending_) {
            return;
        }
        pending_ = false;
        do_flush = true;
    }
    cv_.notify_all();
    if (do_flush) {
        try {
            fn_();
        } catch (const std::exception& e) {
            Logger::instance().error(std::string("DelayedFileWriter immediate write failed: ") + e.what());
        } catch (...) {
            Logger::instance().error("DelayedFileWriter immediate write failed: unknown exception");
        }
    }
}

void DelayedFileWriter::cancel() {
    {
        std::lock_guard lock(mutex_);
        pending_ = false;
    }
    cv_.notify_all();
}

void DelayedFileWriter::run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        // Wait until a write is scheduled (or shutdown).
        cv_.wait(lock, [this] { return stop_ || pending_; });
        if (stop_) {
            break;
        }

        // Debounce: wait out the delay, but restart it if a new write() lands
        // while we wait (generation changes).
        const uint64_t gen = generation_;
        cv_.wait_for(lock, delay_, [this, gen] {
            return stop_ || !pending_ || generation_ != gen;
        });
        if (stop_) {
            break;
        }
        if (!pending_) {
            continue;  // cancelled (or flushed synchronously) while waiting
        }
        if (generation_ != gen) {
            continue;  // a newer write() arrived -> restart the delay
        }

        pending_ = false;
        lock.unlock();
        try {
            fn_();
        } catch (const std::exception& e) {
            Logger::instance().error(std::string("DelayedFileWriter callback failed: ") + e.what());
        } catch (...) {
            Logger::instance().error("DelayedFileWriter callback failed: unknown exception");
        }
        lock.lock();
    }
}

}  // namespace engine::profile