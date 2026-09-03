// Unit tests for the engine::Network:: gateway - redaction, the FakeNetwork
// record-keeping contract, and the ring-buffer snapshot. Avoids any live
// network traffic so it can run anywhere ctest does.
#include "engine/network/network_manager.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using engine::network::FakeNetworkManager;
using engine::network::NetworkOptions;

TEST_CASE("redaction: sensitive header names are scrubbed", "[engine][network]") {
  REQUIRE(engine::network::redaction::is_sensitive_header("apikey"));
  REQUIRE(engine::network::redaction::is_sensitive_header("Authorization"));
  REQUIRE(engine::network::redaction::is_sensitive_header("cookie"));
  REQUIRE(engine::network::redaction::is_sensitive_header("Set-Cookie"));
  REQUIRE(engine::network::redaction::is_sensitive_header("X-API-Key"));
  REQUIRE(engine::network::redaction::is_sensitive_header("x-auth-token"));

  REQUIRE_FALSE(engine::network::redaction::is_sensitive_header("Accept"));
  REQUIRE_FALSE(engine::network::redaction::is_sensitive_header("User-Agent"));
  REQUIRE_FALSE(engine::network::redaction::is_sensitive_header("Content-Type"));
}

TEST_CASE("redaction: header lines", "[engine][network]") {
  REQUIRE(engine::network::redaction::redact_header_line("Accept: application/json") ==
          "Accept: application/json");
  REQUIRE(engine::network::redaction::redact_header_line("Authorization: Bearer xyz") ==
          "Authorization: <redacted>");
  REQUIRE(engine::network::redaction::redact_header_line("apikey: SECRET") == "apikey: <redacted>");
  REQUIRE(engine::network::redaction::redact_header_line("cookie: a=1; b=2") ==
          "cookie: <redacted>");
}

TEST_CASE("redaction: query string", "[engine][network]") {
  // Sensitive params are redacted, others pass through verbatim.
  REQUIRE(engine::network::redaction::redact_url("https://api.nexusmods.com/foo?key=ABC&bar=1") ==
          "https://api.nexusmods.com/foo?key=<redacted>&bar=1");
  // Multiple sensitive keys.
  REQUIRE(engine::network::redaction::redact_url(
              "https://x/?token=ABC&apikey=DEF&page=2") ==
          "https://x/?token=<redacted>&apikey=<redacted>&page=2");
  // No query string -> unchanged.
  REQUIRE(engine::network::redaction::redact_url("https://x/path") == "https://x/path");
}

TEST_CASE("redaction: body", "[engine][network]") {
  REQUIRE(engine::network::redaction::redact_body("apikey=ABC&foo=bar", 4096) ==
          "apikey=<redacted>&foo=bar");
  REQUIRE(engine::network::redaction::redact_body("{\"apikey\":\"ABC\",\"name\":\"x\"}", 4096) ==
          "{\"apikey\":\"<redacted>\",\"name\":\"x\"}");
  // Plain body with no '=' -> unchanged.
  REQUIRE(engine::network::redaction::redact_body("hello world", 4096) == "hello world");
}

TEST_CASE("redaction: cookie blob", "[engine][network]") {
  REQUIRE(engine::network::redaction::redact_cookie("") == std::string());
  REQUIRE(engine::network::redaction::redact_cookie("a=1; b=2; c=3") ==
          "a=<redacted>, ... (13 chars)");
}

TEST_CASE("FakeNetworkManager: records requests and serves canned responses",
          "[engine][network]") {
  FakeNetworkManager fake;
  engine::network::Response canned;
  canned.http_code = 200;
  canned.body = "{\"ok\":true}";
  fake.enqueue_response(canned);

  engine::network::Request req;
  req.url = "https://api.example.com/v1/test";
  req.caller = "TEST";
  req.headers.push_back("apikey: SECRET");

  auto resp = fake.request(req);
  REQUIRE(resp.error.empty());
  REQUIRE(resp.http_code == 200);
  REQUIRE(resp.body == "{\"ok\":true}");
  REQUIRE(fake.seen_requests().size() == 1);
  REQUIRE(fake.seen_requests()[0].caller == "TEST");
  REQUIRE(fake.seen_requests()[0].url == "https://api.example.com/v1/test");
}

TEST_CASE("FakeNetworkManager: offline short-circuits with error",
          "[engine][network]") {
  FakeNetworkManager fake;
  fake.set_offline(true);
  engine::network::Request req;
  req.url = "https://x/";
  req.caller = "TEST";
  auto resp = fake.request(req);
  REQUIRE(resp.error == "offline");
}

TEST_CASE("FakeNetworkManager: empty queue surfaces no-fake-response error",
          "[engine][network]") {
  FakeNetworkManager fake;
  engine::network::Request req;
  req.url = "https://x/";
  req.caller = "TEST";
  auto resp = fake.request(req);
  REQUIRE(resp.error == "no fake response queued");
}

TEST_CASE("FakeNetworkManager: download returns ok and records the call",
          "[engine][network]") {
  FakeNetworkManager fake;
  engine::network::DownloadRequest req;
  req.url = "https://x/file.7z";
  req.caller = "TEST";
  req.dest = std::filesystem::temp_directory_path() / "gmm_test_dl";
  auto r = fake.download(req);
  REQUIRE(r.ok);
  REQUIRE(r.http_code == 200);
  REQUIRE(fake.seen_downloads().size() == 1);
  REQUIRE(fake.seen_downloads()[0].url == "https://x/file.7z");
}

TEST_CASE("FakeNetworkManager: options round-trip", "[engine][network]") {
  FakeNetworkManager fake;
  NetworkOptions opts;
  opts.offline_mode = true;
  opts.use_proxy = true;
  opts.proxy_host = "127.0.0.1";
  opts.proxy_port = 8888;
  opts.max_parallel = 4;
  fake.set_options(opts);
  auto got = fake.options();
  REQUIRE(got.offline_mode);
  REQUIRE(got.use_proxy);
  REQUIRE(got.proxy_host == "127.0.0.1");
  REQUIRE(got.proxy_port == 8888);
  REQUIRE(got.max_parallel == 4);
  REQUIRE(fake.is_offline());
}

TEST_CASE("FakeNetworkManager: snapshot/queue_depth/active are inert",
          "[engine][network]") {
  FakeNetworkManager fake;
  REQUIRE(fake.active_requests().empty());
  REQUIRE(fake.queue_depth() == 0);
  REQUIRE(fake.log_snapshot().empty());
  fake.clear_log();
  fake.cancel_all();
}

TEST_CASE("NetworkOptions: defaults are sane", "[engine][network]") {
  NetworkOptions opts;
  REQUIRE_FALSE(opts.offline_mode);
  REQUIRE_FALSE(opts.use_proxy);
  REQUIRE(opts.proxy_host.empty());
  REQUIRE(opts.proxy_port == 8080);
  REQUIRE(opts.max_parallel == 2);
  REQUIRE(opts.nexus_queue_downloads);
  REQUIRE(opts.workshop_rate_limit_per_hour == 60);
  REQUIRE(opts.default_timeout_seconds == 30);
  REQUIRE(opts.max_retries == 0);
}