// Tests for the Nexus v2 GraphQL client (src/engine/network/nexus_v2/).
// HTTP is faked (FakeNetworkManager); backoff sleeps are recorded, never
// real. Auth disk state is redirected via XDG_CONFIG_HOME like keyring_test.
#include "engine/network/nexus_v2/client.h"
#include "engine/network/nexus_v2/collection_revision.h"
#include "engine/network/nexus_v2/premium.h"

#include "engine/network/network_manager.h"
#include "engine/source/nexus/auth.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using engine::network::FakeNetworkManager;
using engine::network::Response;
using engine::nexus_v2::Client;

namespace {

Response fake_http(long code, const std::string &body,
                   const std::string &headers = {}) {
  Response r;
  r.http_code        = code;
  r.body             = body;
  r.response_headers = headers;
  return r;
}

const char *kEnvelope =
    R"({"data": {"collectionRevision": {"id": 7, "revisionNumber": 3, "revisionStatus": "is_public",)"
    R"( "collection": {"id": 42, "name": "Test Collection", "slug": "test-collection",)"
    R"( "game": {"domainName": "skyrimspecialedition"}},)"
    R"( "modFiles": [)"
    R"( {"gameId": 1704, "fileId": 19080, "version": "0.4.20", "optional": false, "updatePolicy": "exact",)"
    R"( "file": {"modId": 17464, "fileId": 19080, "name": "RaceMenu.7z", "version": "0.4.20", "size": 12345}},)"
    R"( {"gameId": 1704, "fileId": 99, "version": "1.0", "optional": true, "updatePolicy": "latest", "file": null})"
    R"( ],)"
    R"( "externalResources": [)"
    R"( {"name": "SKSE", "resourceUrl": "https://example.com/skse.7z", "version": "2.2", "optional": false})"
    R"( ]}}})";

}  // namespace

TEST_CASE("nexus v2 graphql client", "[engine]") {
  // Redirect Auth disk state before the singleton is first touched.
  const fs::path config =
      fs::temp_directory_path() / ("gmm_nexus_v2_test_" + std::to_string(getpid()));
  fs::create_directories(config);
  setenv("XDG_CONFIG_HOME", config.c_str(), 1);

  SECTION("build_body embeds query and variables") {
    const std::string body = Client::build_body("query X { y }", R"({"a": 1})");
    CHECK(body.find("query X { y }") != std::string::npos);
    CHECK(body.find("\"a\":1") != std::string::npos);
    const std::string empty = Client::build_body("q", "");
    CHECK(empty.find("\"variables\":{}") != std::string::npos);
    const std::string garbage = Client::build_body("q", "not json");
    CHECK(garbage.find("\"variables\":{}") != std::string::npos);
  }

  SECTION("backoff delays double and cap") {
    CHECK(Client::backoff_delay_ms(0, 0) == 1000);
    CHECK(Client::backoff_delay_ms(1, 0) == 2000);
    CHECK(Client::backoff_delay_ms(2, 0) == 4000);
    CHECK(Client::backoff_delay_ms(3, 0) == 8000);
    CHECK(Client::backoff_delay_ms(10, 0) == 8000);
    CHECK(Client::backoff_delay_ms(0, 2500) == 2500);
    CHECK(Client::backoff_delay_ms(0, 99999999) == 8000);
    CHECK(Client::backoff_delay_ms(0, -5) == 1000);
  }

  SECTION("parse_response extracts data root") {
    std::string data, error;
    CHECK(Client::parse_response(R"({"data": {"collectionRevision": {"a": 1}}})",
                                 "collectionRevision", data, error));
    CHECK(data == R"({"a":1})");
    CHECK(error.empty());
  }

  SECTION("parse_response joins graphql errors") {
    std::string data, error;
    CHECK_FALSE(Client::parse_response(
        R"({"errors": [{"message": "bad slug"}, {"message": "nope"}]})",
        "collectionRevision", data, error));
    CHECK(error == "bad slug, nope");
    CHECK(data.empty());
  }

  SECTION("parse_response rejects garbage and empty") {
    std::string data, error;
    CHECK_FALSE(Client::parse_response("not json", "collectionRevision", data, error));
    CHECK_FALSE(
        Client::parse_response(R"({"data": null})", "collectionRevision", data, error));
    CHECK_FALSE(Client::parse_response(R"({"data": {"collectionRevision": null}})",
                                       "collectionRevision", data, error));
  }

  SECTION("query posts to v2 endpoint with apikey") {
    FakeNetworkManager net;
    net.enqueue_response(
        fake_http(200, R"({"data": {"collectionRevision": {"id": 1}}})"));
    std::vector<int> sleeps;
    Client client(
        net,
        [] {
          return "test-key";
        },
        [&](int ms) {
          sleeps.push_back(ms);
        });
    auto res = client.query("collectionRevision", "query Q { x }", "{}");
    CHECK(res.ok);
    CHECK(res.attempts == 1);
    CHECK(sleeps.empty());
    REQUIRE(net.seen_requests().size() == 1);
    const auto &req = net.seen_requests().front();
    CHECK(req.url == engine::nexus_v2::kEndpoint);
    bool has_key = false, has_json = false;
    for (const auto &h : req.headers) {
      if (h == "apikey: test-key")
        has_key = true;
      if (h == "Content-Type: application/json")
        has_json = true;
    }
    CHECK(has_key);
    CHECK(has_json);
    CHECK(req.body.find("query Q { x }") != std::string::npos);
  }

  SECTION("query retries 429 then succeeds") {
    FakeNetworkManager net;
    net.enqueue_response(fake_http(429, "slow down"));
    net.enqueue_response(fake_http(200, R"({"data": {"r": {"id": 1}}})"));
    std::vector<int> sleeps;
    Client client(
        net,
        [] {
          return "k";
        },
        [&](int ms) {
          sleeps.push_back(ms);
        });
    auto res = client.query("r", "q", "{}");
    CHECK(res.ok);
    CHECK(res.attempts == 2);
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps.front() == 1000);
  }

  SECTION("query honors Retry-After on 429") {
    FakeNetworkManager net;
    net.enqueue_response(fake_http(429, "slow", "HTTP/1.1 429\r\nRetry-After: 2\r\n"));
    net.enqueue_response(fake_http(200, R"({"data": {"r": 1}})"));
    std::vector<int> sleeps;
    Client client(
        net,
        [] {
          return "k";
        },
        [&](int ms) {
          sleeps.push_back(ms);
        });
    auto res = client.query("r", "q", "{}");
    CHECK(res.ok);
    REQUIRE(sleeps.size() == 1);
    CHECK(sleeps.front() == 2000);
  }

  SECTION("query gives up after max attempts") {
    FakeNetworkManager net;
    for (int i = 0; i < Client::kMaxAttempts; ++i)
      net.enqueue_response(fake_http(429, "slow"));
    std::vector<int> sleeps;
    Client client(
        net,
        [] {
          return "k";
        },
        [&](int ms) {
          sleeps.push_back(ms);
        });
    auto res = client.query("r", "q", "{}");
    CHECK_FALSE(res.ok);
    CHECK(res.attempts == Client::kMaxAttempts);
    CHECK(sleeps.size() == static_cast<std::size_t>(Client::kMaxAttempts - 1));
    CHECK(res.error.find("429") != std::string::npos);
  }

  SECTION("query never retries auth rejection or server errors") {
    for (long code : {401L, 403L, 500L}) {
      FakeNetworkManager net;
      net.enqueue_response(fake_http(code, "nope"));
      Client client(
          net,
          [] {
            return "k";
          },
          [](int) {
            FAIL("must not sleep");
          });
      auto res = client.query("r", "q", "{}");
      CHECK_FALSE(res.ok);
      CHECK(res.attempts == 1);
    }
  }

  SECTION("query surfaces transport errors and graphql errors") {
    FakeNetworkManager net;
    Response transport;
    transport.error = "boom";
    net.enqueue_response(std::move(transport));
    Client client(
        net,
        [] {
          return "k";
        },
        [](int) {});
    auto fail = client.query("r", "q", "{}");
    CHECK_FALSE(fail.ok);
    CHECK(fail.error.find("boom") != std::string::npos);

    FakeNetworkManager net2;
    net2.enqueue_response(fake_http(200, R"({"errors": [{"message": "bad query"}]})"));
    Client client2(
        net2,
        [] {
          return "k";
        },
        [](int) {
          FAIL("must not sleep");
        });
    auto gql_err = client2.query("r", "q", "{}");
    CHECK_FALSE(gql_err.ok);
    CHECK(gql_err.error == "bad query");
  }

  SECTION("query without api key makes no request") {
    FakeNetworkManager net;
    Client client(
        net,
        [] {
          return "";
        },
        [](int) {
          FAIL("must not sleep");
        });
    auto res = client.query("r", "q", "{}");
    CHECK_FALSE(res.ok);
    CHECK(res.attempts == 0);
    CHECK(net.seen_requests().empty());
  }

  SECTION("revision query text and variables") {
    const std::string q = engine::nexus_v2::build_collection_revision_query();
    CHECK(q.find("collectionRevision") != std::string::npos);
    CHECK(q.find("$slug: String!") != std::string::npos);
    CHECK(q.find("modFiles") != std::string::npos);
    const std::string latest =
        engine::nexus_v2::build_collection_revision_variables("slug-a", 0);
    CHECK(latest.find("slug-a") != std::string::npos);
    CHECK(latest.find("revision") == std::string::npos);
    const std::string pinned =
        engine::nexus_v2::build_collection_revision_variables("slug-a", 5);
    CHECK(pinned.find("\"revision\":5") != std::string::npos);
  }

  SECTION("parse revision fixture") {
    std::string node, error;
    REQUIRE(Client::parse_response(kEnvelope, "collectionRevision", node, error));
    engine::nexus_v2::CollectionRevision rev;
    REQUIRE(engine::nexus_v2::parse_collection_revision_data(node, rev, error));
    CHECK(rev.found);
    CHECK(rev.name == "Test Collection");
    CHECK(rev.slug == "test-collection");
    CHECK(rev.revision_number == 3);
    CHECK(rev.revision_status == "is_public");
    CHECK(rev.collection_id == 42);
    CHECK(rev.game_domain == "skyrimspecialedition");
    REQUIRE(rev.mods.size() == 2);
    CHECK(rev.mods[0].mod_id == 17464);
    CHECK(rev.mods[0].file_id == 19080);
    CHECK(rev.mods[0].file_name == "RaceMenu.7z");
    CHECK(rev.mods[0].update_policy == "exact");
    CHECK_FALSE(rev.mods[0].optional);
    // file: null stays, mod_id unknown - tolerated, not dropped.
    CHECK(rev.mods[1].mod_id == 0);
    CHECK(rev.mods[1].file_id == 99);
    CHECK(rev.mods[1].optional);
    REQUIRE(rev.external_resources.size() == 1);
    CHECK(rev.external_resources[0].url == "https://example.com/skse.7z");
  }

  SECTION("parse revision rejects null and garbage") {
    engine::nexus_v2::CollectionRevision rev;
    std::string error;
    CHECK_FALSE(engine::nexus_v2::parse_collection_revision_data("null", rev, error));
    CHECK_FALSE(
        engine::nexus_v2::parse_collection_revision_data("garbage{", rev, error));
    CHECK_FALSE(engine::nexus_v2::parse_collection_revision_data("[1,2]", rev, error));
    CHECK_FALSE(error.empty());
  }

  SECTION("fetch revision end to end over fake") {
    FakeNetworkManager net;
    net.enqueue_response(fake_http(200, kEnvelope));
    Client client(
        net,
        [] {
          return "k";
        },
        [](int) {});
    auto res =
        engine::nexus_v2::fetch_collection_revision(client, "test-collection", 3);
    CHECK(res.ok);
    CHECK(res.error.empty());
    CHECK(res.revision.mods.size() == 2);
    REQUIRE(net.seen_requests().size() == 1);
    CHECK(net.seen_requests().front().body.find("test-collection") !=
          std::string::npos);
  }

  SECTION("fetch revision surfaces missing collection") {
    FakeNetworkManager net;
    net.enqueue_response(fake_http(200, R"({"data": {"collectionRevision": null}})"));
    Client client(
        net,
        [] {
          return "k";
        },
        [](int) {});
    auto res = engine::nexus_v2::fetch_collection_revision(client, "nope", 1);
    CHECK_FALSE(res.ok);
    CHECK_FALSE(res.error.empty());
  }

  SECTION("premium detection follows stored account type") {
    using AccountType = engine::Source::Nexus::NexusUserInfo::AccountType;
    auto &auth        = engine::Source::Nexus::Auth::instance();
    auth.clear_user_info();
    CHECK_FALSE(engine::nexus_v2::is_premium_user());
    engine::Source::Nexus::NexusUserInfo info;
    info.account_type = AccountType::Regular;
    auth.set_user_info(info);
    CHECK_FALSE(engine::nexus_v2::is_premium_user());
    info.account_type = AccountType::Supporter;
    auth.set_user_info(info);
    CHECK_FALSE(engine::nexus_v2::is_premium_user());
    info.account_type = AccountType::Premium;
    auth.set_user_info(info);
    CHECK(engine::nexus_v2::is_premium_user());
    auth.clear_user_info();
    CHECK_FALSE(engine::nexus_v2::is_premium_user());
  }
}
