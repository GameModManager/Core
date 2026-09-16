#pragma once

// Nexus v2 GraphQL client - thin POST wrapper over engine::network::Interface.
//
// Separate from the v1 REST client (engine/source/nexus_*): v2 speaks GraphQL
// (https://api.nexusmods.com/v2/graphql) for Collections data the v1 REST API
// does not expose. All HTTP goes through network::Interface so tests inject
// FakeNetworkManager and production gets proxy/offline/logging for free.
// Qt-free: std::function injection only, no Qt types.

#include <functional>
#include <string>

namespace engine::network
{
class Interface;
struct Response;
}  // namespace engine::network

namespace engine::nexus_v2
{

// GraphQL endpoint (mirrors node-nexus-api GRAPHQL_URL).
inline const char* kEndpoint = "https://api.nexusmods.com/v2/graphql";

// Result of one Client::query call (after any 429 retries).
struct GraphqlResult
{
  bool ok        = false;
  long http_code = 0;
  // Re-serialized JSON of data.<root> on success, empty otherwise.
  std::string data;
  // Human-readable failure reason, empty on success.
  std::string error;
  // How many HTTP attempts were made (1 when no retry happened).
  int attempts = 0;
};

class Client
{
public:
  using ApiKeyProvider = std::function<std::string()>;
  using Sleeper        = std::function<void(int)>;

  // net: the HTTP gateway (production: network::instance(), tests: fake).
  // api_key: defaults to the stored Nexus key (Auth singleton); tests pass
  //   a lambda. sleeper: defaults to a real sleep; tests record delays.
  explicit Client(network::Interface& net, ApiKeyProvider api_key = {},
                  Sleeper sleeper = {});

  // POST {"query": ..., "variables": ...} and return data.<root>.
  // Retries 429s with exponential backoff (honors Retry-After, see
  // backoff_delay_ms). 401/403 and GraphQL errors[] never retry.
  // Never throws.
  GraphqlResult query(const std::string& root, const std::string& query_text,
                      const std::string& variables_json);

  // Pure helpers (unit-tested, no I/O).
  static std::string build_body(const std::string& query_text,
                                const std::string& variables_json);
  // attempt is 0-based. retry_after_ms <= 0 means "no header present".
  static int backoff_delay_ms(int attempt, int retry_after_ms);
  // Split a GraphQL envelope into data.<root> or a joined errors[] message.
  // Returns true on success (out_data set), false on error (out_error set).
  static bool parse_response(const std::string& body, const std::string& root,
                             std::string& out_data, std::string& out_error);

  static constexpr int kMaxAttempts = 4;
  static constexpr int kBaseDelayMs =
      1000;  // mirrors node-nexus-api DELAY_AFTER_429_MS
  static constexpr int kMaxDelayMs = 8000;

private:
  network::Interface& net_;
  ApiKeyProvider api_key_;
  Sleeper sleeper_;
};

}  // namespace engine::nexus_v2
