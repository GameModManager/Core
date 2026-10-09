#include "engine/log/logger.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

using namespace engine;

namespace {
void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("logger", "[engine]") {
  auto &logger = Logger::instance();
  logger.set_log_file("/dev/null");  // avoid writing into the cwd

  // --- pre-subscription messages are replayed in order ---
  logger.info("before-a");
  logger.warn("before-b");
  logger.error("before-c");

  std::vector<std::string> replayed;
  logger.add_callback(
      [&replayed](LogLevel level, const std::string &ts, const std::string &msg) {
        (void)level;
        (void)ts;
        replayed.push_back(msg);
      });

  require(replayed.size() == 3, "pre-subscription messages replayed");
  require(replayed[0] == "before-a", "replay order preserved (1)");
  require(replayed[1] == "before-b", "replay order preserved (2)");
  require(replayed[2] == "before-c", "replay order preserved (3)");

  // --- post-subscription messages are delivered live ---
  logger.debug("live-dbg");
  logger.info("live-inf");
  require(replayed.size() == 5, "post-subscription messages delivered live");

  // --- level values are preserved through replay ---
  std::vector<int> levels;
  logger.add_callback(
      [&levels](LogLevel level, const std::string &, const std::string &) {
        levels.push_back(static_cast<int>(level));
      });
  require(levels.size() == 5, "second subscriber replayed too");
  require(levels[3] == static_cast<int>(LogLevel::Debug), "replayed level preserved");
  require(levels[4] == static_cast<int>(LogLevel::Info),
          "replayed level preserved (2)");

  // --- buffer is bounded ---
  logger.set_level(LogLevel::Debug);
  for (int i = 0; i < 300; ++i) {
    logger.debug("flood-" + std::to_string(i));
  }
  std::vector<std::string> tail;
  logger.add_callback(
      [&tail](LogLevel level, const std::string &, const std::string &msg) {
        (void)level;
        tail.push_back(msg);
      });
  require(tail.size() == 256, "replay buffer bounded at kReplayLimit");
  require(tail.front() == "flood-44", "oldest buffered entry is flood-44");
  require(tail.back() == "flood-299", "newest buffered entry is flood-299");
}

TEST_CASE("logger set_log_file reports whether it opened", "[engine]") {
  auto &logger = Logger::instance();

  // A writable path opens, and the path is recorded either way so
  // "open the logs folder" still has somewhere to point.
  const std::string good = "/tmp/gmm-logger-open-test.log";
  std::remove(good.c_str());
  REQUIRE(logger.set_log_file(good));
  REQUIRE(logger.log_file_path() == good);
  REQUIRE(logger.log_dir() == "/tmp");

  // An unopenable path (a directory that does not exist, so O_CREAT cannot
  // make the file) reports failure. Without this the app logs into a void and
  // the user is never told - which is the bug the return value exists for.
  const std::string bad = "/tmp/gmm-no-such-dir-8f3a/log.txt";
  std::error_code ignored;
  std::filesystem::remove_all("/tmp/gmm-no-such-dir-8f3a", ignored);
  REQUIRE(!logger.set_log_file(bad));
  // Still recorded: the failure is about opening, not about the location.
  REQUIRE(logger.log_file_path() == bad);

  // Leave the logger on a real file so a later test in this binary does not
  // inherit the failure.
  REQUIRE(logger.set_log_file(good));
  std::remove(good.c_str());
}
