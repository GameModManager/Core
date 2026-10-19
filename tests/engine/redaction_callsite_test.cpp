// A call site that logs a signed URL must not undo the redaction the Network
// facade already applies. downloads_controller.cpp strips csrfKey= from its own
// log line and then hands the original URL to the provider, which reaches
// curl_download - so without the redactor at that call site the value lands in
// gamemodmanager.log anyway.
//
// Both download call sites are covered, not just curl_download: Network::Manager
// is the other one, and it is the one a transfer actually starts from.
//
// These are negative tests: they assert the secret is ABSENT from the captured
// log, not that a redaction call was made. A redaction call that was later
// dropped, or a URL field added to a new log line, would leave the secret
// present and fail here.
#include "engine/github.h"
#include "engine/log/logger.h"
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

// The SECOND download call site, and the one that was actually leaking.
//
// curl_download is not the only thing that opens a download: Network::Manager
// (downloadmanager.cpp and the Nexus/LoversLab providers' long-lived transfers)
// is, and Manager::download logs its request line before it builds a handle.
// That line used req.url raw, so a Nexus download_link URL - whose `key=` is
// the one-shot download token - reached gamemodmanager.log even though every
// LogEntry it records five lines later carries redact_url's output.
//
// No bytes move: cancel_all() is the documented kill switch, and download()
// consults it right AFTER the log line under test, so the assertion is on the
// shipped line and the run never opens a socket.
TEST_CASE("Network::Manager::download never logs a signed URL raw",
          "[engine][redaction]") {
  const std::string kKey = "SUPER-SECRET-DOWNLOAD-TOKEN-7e4d2c";
  const std::string url =
      "https://nexus.nexusmods.com/skyrimspecialedition/mods/1234/files/5678?key=" +
      kKey + "&expires=1700000000";

  engine::network::Manager mgr;
  mgr.cancel_all();

  engine::network::DownloadRequest req;
  req.url    = url;
  req.caller = "redaction_callsite_test";
  req.dest   = fs::temp_directory_path() / "gmm_redact_mgr_dest.bin";

  {
    LogCapture cap;
    const auto res = mgr.download(req);
    // The kill switch short-circuited, which is what proves nothing was
    // fetched - if this ever becomes ok=true the case is testing the network,
    // not the log line.
    CHECK_FALSE(res.ok);
    CHECK(res.error.find("cancelled") != std::string::npos);
    CHECK(cap.lacks(kKey));
    CHECK(cap.contains("[Network::Manager] download:"));
    CHECK(cap.contains("key=<redacted>"));
  }

  std::error_code ec;
  fs::remove(req.dest, ec);
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

  // The modl direct-URL call site used to truncate the whole query string,
  // which covered ?signature= too. The shared redactor must keep that
  // coverage or the swap loses a secret the old code protected.
  const std::string modl = redact_url(
      "https://mod.pub/files/1234/download?expires=1700000000&signature=SIGVALUE99");
  CHECK(modl.find("SIGVALUE99") == std::string::npos);
  CHECK(modl.find("signature=<redacted>") != std::string::npos);
  CHECK(modl.find("expires=1700000000") != std::string::npos);
}

// The redirect half of the class: CURLINFO_EFFECTIVE_URL is the final URL
// after redirects, and a redirect Location can carry a token just as well as
// the request URL can. The LogEntry it lands in used to carry it raw.
//
// No socket is opened: the URL points at a port on localhost that refuses the
// connection immediately, which still records a LogEntry whose effective_url
// is the requested URL.
TEST_CASE("Network::Manager::request redacts the effective URL in its LogEntry",
          "[engine][redaction]") {
  const std::string kKey = "SUPER-SECRET-REDIRECT-TOKEN-4b8a1d";
  const std::string url  = "http://127.0.0.1:1/some/path?key=" + kKey + "&expires=1";

  engine::network::Manager mgr;
  engine::network::Request req;
  req.url    = url;
  req.caller = "redaction_callsite_test";

  {
    LogCapture cap;
    const auto resp = mgr.request(req);
    CHECK(resp.http_code == 0);  // connection refused, no HTTP response
    const auto entries = mgr.log_snapshot(100);
    CHECK(entries.size() > 0);
    for (const auto &e : entries)
      CHECK(e.effective_url.find(kKey) == std::string::npos);
    // The redactor ran: the recorded effective URL carries the placeholder.
    bool saw_placeholder = false;
    for (const auto &e : entries)
      if (e.effective_url.find("key=<redacted>") != std::string::npos)
        saw_placeholder = true;
    CHECK(saw_placeholder);
    CHECK(cap.lacks(kKey));
  }
}

TEST_CASE("Network::Manager::download redacts the effective URL in its LogEntry",
          "[engine][redaction]") {
  const std::string kKey = "SUPER-SECRET-REDIRECT-TOKEN-9c0e2f";
  const std::string url  = "http://127.0.0.1:1/some/file?key=" + kKey + "&expires=1";

  engine::network::Manager mgr;
  engine::network::DownloadRequest req;
  req.url    = url;
  req.caller = "redaction_callsite_test";
  req.dest   = fs::temp_directory_path() / "gmm_redact_eff_dest.bin";

  {
    LogCapture cap;
    const auto res = mgr.download(req);
    CHECK_FALSE(res.ok);
    const auto entries = mgr.log_snapshot(100);
    CHECK(entries.size() > 0);
    for (const auto &e : entries)
      CHECK(e.effective_url.find(kKey) == std::string::npos);
    bool saw_placeholder = false;
    for (const auto &e : entries)
      if (e.effective_url.find("key=<redacted>") != std::string::npos)
        saw_placeholder = true;
    CHECK(saw_placeholder);
    CHECK(cap.lacks(kKey));
  }

  std::error_code ec;
  fs::remove(req.dest, ec);
}

// The GitHub error lines log the request URL next to the curl error string.
// The URL comes from server-provided asset data, so it goes through the
// redactor like every other URL log line. The fake manager fails the request
// without touching the network, which is what drives both error paths.
TEST_CASE("GitHub error lines never log the URL raw", "[engine][redaction]") {
  const std::string kToken = "SUPER-SECRET-GITHUB-TOKEN-5d7f3a";
  const std::string url =
      "https://github.com/owner/repo/releases/download/v1.0/file.7z?token=" + kToken;

  auto fake = std::make_unique<engine::network::FakeNetworkManager>();
  fake->set_offline(true);
  engine::network::set_instance(std::move(fake));

  {
    LogCapture cap;
    const auto rel = engine::GitHub::latest_release("owner", "repo", false);
    CHECK_FALSE(rel.has_value());
    CHECK(cap.contains("GitHub API GET failed:"));
    // latest_release builds its own api.github.com URL (no token in it); the
    // token-bearing URL below is what exercises the redactor.
    CHECK(cap.lacks(kToken));

    const fs::path dest = fs::temp_directory_path() / "gmm_redact_gh_dest.bin";
    const bool ok       = engine::GitHub::download(url, dest, {});
    CHECK_FALSE(ok);
    CHECK(cap.contains("GitHub download failed:"));
    CHECK(cap.lacks(kToken));
    CHECK(cap.contains("token=<redacted>"));
    std::error_code ec;
    fs::remove(dest, ec);
  }

  engine::network::set_instance(nullptr);
}