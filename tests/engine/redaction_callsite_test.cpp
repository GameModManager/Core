// A call site that logs a signed URL must not undo the redaction the Network
// facade already applies. downloads_controller.cpp strips csrfKey= from its own
// log line and then hands the original URL to the provider, which reaches
// curl_download - so without the redactor at that call site the value lands in
// gamemodmanager.log anyway.
//
// These are negative tests: they assert the secret is ABSENT from the captured
// log, not that a redaction call was made. A redaction call that was later
// dropped, or a URL field added to a new log line, would leave the secret
// present and fail here.
#include "engine/core/log/logger.h"
#include "engine/network/network_manager.h"
#include "engine/source/download/curl_download.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Captures every log line emitted while it is in scope.
//
// Logger::add_callback is append-only and holds its callback for the life of
// the process, so a lambda capturing `this` would dangle once the LogCapture
// goes out of scope and later test cases would write into freed memory. The
// callback is therefore stateless: it pushes into whichever capture is
// currently registered, and ignores everything when none is.
class LogCapture {
public:
  static std::vector<std::string> *active_;

  LogCapture() {
    if (active_)
      FAIL("a LogCapture is already registered");
    active_ = &lines_;
    engine::Logger::instance().add_callback(
        [](engine::LogLevel, const std::string &, const std::string &msg) {
          if (active_)
            active_->push_back(msg);
        });
  }
  ~LogCapture() { active_ = nullptr; }

  LogCapture(const LogCapture &)            = delete;
  LogCapture &operator=(const LogCapture &) = delete;

  bool contains(const std::string &needle) const {
    for (const auto &l : lines_)
      if (l.find(needle) != std::string::npos)
        return true;
    return false;
  }

  // True when no captured line holds the needle - the property under test.
  bool lacks(const std::string &needle) const { return !contains(needle); }

  std::size_t count() const { return lines_.size(); }

private:
  std::vector<std::string> lines_;
};

std::vector<std::string> *LogCapture::active_ = nullptr;

}  // namespace

TEST_CASE("curl_download never logs a signed URL raw", "[engine][redaction]") {
  const std::string kCsrf = "SUPER-SECRET-CSRF-VALUE-9f3a2b";
  const std::string url =
      "https://www.loverslab.com/files/file/4242-slug/?do=download&csrfKey=" +
      kCsrf + "&r=7";

  // Route through the real curl_download entry point so the assertion is on the
  // shipped log line, not on a re-implementation of it.
  auto fake = std::make_unique<engine::network::FakeNetworkManager>();
  engine::network::set_instance(std::move(fake));

  const fs::path dest = fs::temp_directory_path() / "gmm_redact_dest.bin";
  long http_code      = 0;
  {
    LogCapture cap;
    engine::download::curl_download(url, dest, http_code);
    CHECK(cap.lacks(kCsrf));
    CHECK(cap.count() > 0);
    CHECK(cap.contains("<redacted>"));
  }

  engine::network::set_instance(nullptr);
  std::error_code ec;
  fs::remove(dest, ec);
}

TEST_CASE("curl_download error path never logs a signed URL raw",
          "[engine][redaction]") {
  const std::string kKey = "SUPER-SECRET-NEXUS-KEY-1a2b3c";
  const std::string url =
      "https://nexus.nexusmods.com/skyrimspecialedition/mods/1234/files/5678?key=" +
      kKey + "&expires=1700000000";

  auto fake = std::make_unique<engine::network::FakeNetworkManager>();
  fake->set_offline(true);  // download() returns ok=false with an error string
  engine::network::set_instance(std::move(fake));

  const fs::path dest = fs::temp_directory_path() / "gmm_redact_dest2.bin";
  long http_code      = 0;
  {
    LogCapture cap;
    engine::download::curl_download(url, dest, http_code);
    CHECK(cap.lacks(kKey));
    CHECK(cap.contains("[curl_download] Error:"));
    CHECK(cap.contains("<redacted>"));
  }

  engine::network::set_instance(nullptr);
}

TEST_CASE("redact_url covers the signed-URL keys the sources use",
          "[engine][redaction]") {
  // The helper the call sites now use already knows these parameter names;
  // this pins the ones the sources actually send so a future edit to the
  // sensitive-name list cannot silently unprotect them.
  using engine::network::redaction::redact_url;

  const std::string csrf = redact_url(
      "https://www.loverslab.com/files/file/1/?do=download&csrfKey=abc123&r=7");
  CHECK(csrf.find("abc123") == std::string::npos);
  CHECK(csrf.find("csrfKey=<redacted>") != std::string::npos);

  const std::string nexus = redact_url(
      "https://nexus.nexusmods.com/skyrimspecialedition/mods/1/files/2?key=deadbeef&"
      "expires=123");
  CHECK(nexus.find("deadbeef") == std::string::npos);
  CHECK(nexus.find("key=<redacted>") != std::string::npos);
  // Non-sensitive params survive so the log stays diagnosable.
  CHECK(nexus.find("expires=123") != std::string::npos);

  const std::string nxm = redact_url(
      "nxm://nexus.nexusmods.com/skyrimspecialedition/mods/1/files/2?key=cafebabe");
  CHECK(nxm.find("cafebabe") == std::string::npos);
  CHECK(nxm.find("skyrimspecialedition") != std::string::npos);
}