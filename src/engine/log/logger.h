#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace engine {

enum class LogLevel { Debug, Info, Warn, Error };

class Logger {
public:
    using Callback = std::function<void(LogLevel, const std::string& timestamp, const std::string& message)>;
    using GroupCallback = std::function<void(bool begin, LogLevel level, const std::string& label)>;

    static Logger& instance();

    void set_level(LogLevel level);
    void add_callback(Callback cb);
    void add_group_callback(GroupCallback cb);
    void log(LogLevel level, const std::string& message);
    void begin_group(LogLevel level, const std::string& label);
    void end_group();

    void debug(const std::string& msg) { log(LogLevel::Debug, msg); }
    void info(const std::string& msg)  { log(LogLevel::Info, msg); }
    void warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
    void error(const std::string& msg) { log(LogLevel::Error, msg); }

    void set_log_file(const std::string& path);
    void enable_console(bool color = true);

private:
    Logger();
    std::string make_timestamp() const;
    std::string level_tag(LogLevel level) const;
    std::string sanitize(std::string msg) const;

    LogLevel min_level_ = LogLevel::Debug;
    std::vector<Callback> callbacks_;
    std::vector<GroupCallback> group_callbacks_;
    std::mutex mutex_;
    int log_fd_ = -1;
    std::string home_dir_;
};

}  // namespace engine
