#include "engine/log/logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace engine {

Logger& Logger::instance() {
    static Logger s;
    return s;
}

void Logger::set_level(LogLevel level) {
    min_level_ = level;
}

void Logger::add_callback(Callback cb) {
    std::lock_guard lock(mutex_);
    callbacks_.push_back(std::move(cb));
}

void Logger::log(LogLevel level, const std::string& message) {
    if (level < min_level_) return;

    auto ts = make_timestamp();

    std::lock_guard lock(mutex_);
    for (auto& cb : callbacks_) {
        cb(level, ts, message);
    }

    if (log_fd_ >= 0) {
        auto tag = level_tag(level);
        std::string line = "[" + tag + "] [" + ts + "] " + message + "\n";
        ::write(log_fd_, line.data(), line.size());
    }
}

void Logger::set_log_file(const std::string& path) {
    std::lock_guard lock(mutex_);
    if (log_fd_ >= 0) ::close(log_fd_);
    log_fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
