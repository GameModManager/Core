#include "engine/log/logger.h"
#include "engine/util/debug_env.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

namespace engine {

Logger::Logger() {
    auto* home = std::getenv("HOME");
    if (home) home_dir_ = home;
}

Logger& Logger::instance() {
    static Logger s;
    return s;
}

void Logger::set_level(LogLevel level) {
    min_level_ = level;
}

void Logger::add_callback(Callback cb) {
    // Replay buffered messages to the new subscriber so it sees messages
    // logged before it registered (e.g. startup lines). Replay goes through
    // the callback itself, so its own level filtering still applies.
    std::vector<LogEntry> replay;
    {
        std::lock_guard lock(mutex_);
        replay.assign(replay_buffer_.begin(), replay_buffer_.end());
        callbacks_.push_back(cb);
    }
    for (const auto& entry : replay) {
        cb(entry.level, entry.timestamp, entry.message);
    }
}

void Logger::add_group_callback(GroupCallback cb) {
    std::lock_guard lock(mutex_);
    group_callbacks_.push_back(std::move(cb));
}

void Logger::begin_group(LogLevel level, const std::string& label) {
    std::lock_guard lock(mutex_);
    for (auto& cb : group_callbacks_) {
        cb(true, level, label);
    }
}

void Logger::end_group() {
    std::lock_guard lock(mutex_);
    for (auto& cb : group_callbacks_) {
        cb(false, LogLevel::Debug, "");
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < min_level_) return;

    auto ts = make_timestamp();
    auto msg = sanitize(message);

    std::lock_guard lock(mutex_);
    for (auto& cb : callbacks_) {
        cb(level, ts, msg);
    }

    if (replay_buffer_.size() >= kReplayLimit) replay_buffer_.pop_front();
    replay_buffer_.push_back({level, ts, msg});

    if (log_fd_ >= 0) {
        auto tag = level_tag(level);
        std::string line = "[" + tag + "] [" + ts + "] " + msg + "\n";
        [[maybe_unused]] auto _ = ::write(log_fd_, line.data(), line.size());
    }
}

void Logger::set_log_file(const std::string& path) {
    std::lock_guard lock(mutex_);
    if (log_fd_ >= 0) ::close(log_fd_);
    log_fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

void Logger::enable_console(bool color) {
    (void)color;
    // Console defaults to Info level (DBG hidden); GMM_DEBUG=1 drops min to Debug.
    const bool verbose = gmm_debug_enabled();
    add_callback([verbose](LogLevel level, const std::string& ts, const std::string& msg) {
        if (!verbose && level < LogLevel::Info) return;
        static const char* colors[] = {
            "\033[90m",  // Debug - bright black
            "\033[0m",   // Info  - default
            "\033[33m",  // Warn  - yellow
            "\033[31m",  // Error - red
        };
        auto tag = level == LogLevel::Debug ? "DBG" :
                   level == LogLevel::Info  ? "INF" :
                   level == LogLevel::Warn  ? "WRN" : "ERR";
        fprintf(stdout, "%s[%s] [%s] %s\033[0m\n",
                colors[static_cast<int>(level)], tag, ts.c_str(), msg.c_str());
        fflush(stdout);
    });
}

void Logger::raw_append(const std::string& line) const {
    if (log_fd_ >= 0)
        ::write(log_fd_, line.data(), line.size());
}

std::string Logger::sanitize(std::string msg) const {
    if (home_dir_.empty()) return msg;
    for (;;) {
        auto pos = msg.find(home_dir_);
        if (pos == std::string::npos) break;
        msg.replace(pos, home_dir_.size(), "{USER}");
    }
    return msg;
}

std::string Logger::make_timestamp() const {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto time_t_now = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    struct tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

std::string Logger::level_tag(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}

}  // namespace engine
