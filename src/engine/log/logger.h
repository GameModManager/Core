#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace engine {

enum class LogLevel { Debug, Info, Warn, Error };

// A single captured log message, kept for late subscribers.
struct LogEntry {
  LogLevel level = LogLevel::Info;
  std::string timestamp;
  std::string message;
};

class Logger {
public:
  using Callback = std::function<void(LogLevel, const std::string &timestamp,
                                      const std::string &message)>;
  using GroupCallback =
      std::function<void(bool begin, LogLevel level, const std::string &label)>;

  static Logger &instance();

  void set_level(LogLevel level);
  void add_callback(Callback cb);
  void add_group_callback(GroupCallback cb);
  void log(LogLevel level, const std::string &message);
  void begin_group(LogLevel level, const std::string &label);
  void end_group();

  void debug(const std::string &msg) { log(LogLevel::Debug, msg); }
  void info(const std::string &msg) { log(LogLevel::Info, msg); }
  void warn(const std::string &msg) { log(LogLevel::Warn, msg); }
  void error(const std::string &msg) { log(LogLevel::Error, msg); }

  // Point the logger at `path`, creating or truncating it. Returns false when
  // the file could not be opened: every later log line is then dropped on the
  // floor with nothing on screen to say why (MO2 parity - loglist.cpp's
  // createAndMakeWritable raises a critical dialog in the same case).
  // The path is still recorded either way, so log_dir() keeps answering for
  // "open the logs folder".
  bool set_log_file(const std::string &path);
  // The path handed to set_log_file, and its parent directory. Empty before
  // the first set_log_file. The log system owns where the log lives, so this
  // is what "open the logs folder" opens - a caller that guesses the location
  // is guessing at a path the log file does not have (it is the data dir, not
  // the cache/crash-dump dir).
  [[nodiscard]] const std::string &log_file_path() const { return log_file_path_; }
  [[nodiscard]] std::string log_dir() const;
  // The replay buffer as it stands now: every message logged so far that
  // survived the kReplayLimit ring, oldest first. A subscriber that filters
  // (a level-filtered log view) re-reads this after the filter changes, so
  // raising the verbosity brings back the lines the previous level hid
  // instead of only ever showing what arrives next.
  [[nodiscard]] std::vector<LogEntry> replayed() const;
  void enable_console(bool color = true);

  // Fork-safe append: writes straight to the log file fd with NO mutex and
  // NO callbacks. Intended for the forked launch supervisor, which must not
  // take the logger's mutex (a GUI thread could have held it at fork) nor
  // run Qt-object callbacks in a child process. Caller supplies the full
  // line including trailing newline.
  void raw_append(const std::string &line) const;

private:
  Logger();
  std::string make_timestamp() const;
  std::string level_tag(LogLevel level) const;
  std::string sanitize(std::string msg) const;

  // Messages replayed to callbacks registered after they were logged.
  static constexpr std::size_t kReplayLimit = 256;

  LogLevel min_level_ = LogLevel::Debug;
  std::vector<Callback> callbacks_;
  std::vector<GroupCallback> group_callbacks_;
  std::deque<LogEntry> replay_buffer_;
  // Mutable so the const observers (log_dir, replayed) can take it; every
  // read of the buffers below goes through it.
  mutable std::mutex mutex_;
  int log_fd_ = -1;
  std::string home_dir_;
  std::string log_file_path_;
};

}  // namespace engine
