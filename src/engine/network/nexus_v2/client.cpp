#include "engine/network/nexus_v2/client.h"

#include "engine/core/log/logger.h"
#include "engine/network/network_manager.h"
#include "engine/source/nexus/account.h"
#include "engine/source/nexus/auth.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace engine::nexus_v2
{

namespace
{

  std::string default_api_key()
  {
    return Source::Nexus::Auth::instance().get_api_key();
  }

  void default_sleeper(int ms)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

}  // namespace

Client::Client(network::Interface& net, ApiKeyProvider api_key, Sleeper sleeper)
    : net_(net),
      api_key_(api_key ? std::move(api_key) : ApiKeyProvider(default_api_key)),
      sleeper_(sleeper ? std::move(sleeper) : Sleeper(default_sleeper))
{}

std::string Client::build_body(const std::string& query_text,
                               const std::string& variables_json)
{
  nlohmann::json body;
  body["query"] = query_text;
  try {
    body["variables"] = variables_json.empty() ? nlohmann::json::object()
                                               : nlohmann::json::parse(variables_json);
  } catch (const std::exception&) {
    body["variables"] = nlohmann::json::object();
  }
  return body.dump();
}

int Client::backoff_delay_ms(int attempt, int retry_after_ms)
{
  if (retry_after_ms > 0)
    return std::min(retry_after_ms, kMaxDelayMs);
  int delay = kBaseDelayMs;
  for (int i = 0; i < attempt && delay < kMaxDelayMs; ++i)
    delay *= 2;
  return std::min(delay, kMaxDelayMs);
}

bool Client::parse_response(const std::string& body, const std::string& root,
                            std::string& out_data, std::string& out_error)
{
  nlohmann::json env;
  try {
    env = nlohmann::json::parse(body);
  } catch (const std::exception& e) {
    out_error = std::string("invalid GraphQL response: ") + e.what();
    return false;
  }
  const auto data_it = env.find("data");
  if (data_it != env.end() && data_it->is_object()) {
    const auto root_it = data_it->find(root);
    if (root_it != data_it->end() && !root_it->is_null()) {
      out_data = root_it->dump();
      return true;
    }
  }
  // No data.<root>: join errors[].message like node-nexus-api genError.
  std::string message;
  const auto err_it = env.find("errors");
  if (err_it != env.end() && err_it->is_array()) {
    for (const auto& err : *err_it) {
      const std::string text = err.value("message", "");
      if (text.empty())
        continue;
      if (!message.empty())
        message += ", ";
      message += text;
    }
  }
  out_error = message.empty() ? "empty GraphQL response" : message;
  return false;
}

GraphqlResult Client::query(const std::string& root, const std::string& query_text,
                            const std::string& variables_json)
{
  GraphqlResult result;
  const std::string key = api_key_();
  if (key.empty()) {
    result.error = "no Nexus API key configured";
    return result;
  }

  network::Request req;
  req.method  = network::Method::Post;
  req.url     = kEndpoint;
  req.headers = {"apikey: " + key, "Content-Type: application/json",
                 "Accept: application/json"};
  req.body    = build_body(query_text, variables_json);
  req.caller  = NET_CALLER;

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    network::Response resp = net_.request(req);
    ++result.attempts;
    result.http_code = resp.http_code;
    // v1 and v2 share the x-rl-* quota headers; keep one parser.
    if (!resp.response_headers.empty())
      Source::Nexus::Account::parse_rate_limits(resp.response_headers);

    if (!resp.error.empty() && resp.http_code == 0) {
      result.error = "request failed: " + resp.error;
      return result;
    }
    if (resp.http_code == 401 || resp.http_code == 403) {
      result.error =
          "API key rejected by Nexus (HTTP " + std::to_string(resp.http_code) + ")";
      return result;
    }
    if (resp.http_code == 429) {
      result.error = "rate limited by Nexus (HTTP 429)";
      if (attempt + 1 >= kMaxAttempts)
        return result;
      sleeper_(backoff_delay_ms(attempt,
                                network::parse_retry_after_ms(resp.response_headers)));
      continue;
    }
    if (resp.http_code < 200 || resp.http_code >= 300) {
      result.error = "Nexus returned HTTP " + std::to_string(resp.http_code);
      return result;
    }
    result.ok = parse_response(resp.body, root, result.data, result.error);
    return result;
  }
  return result;
}

}  // namespace engine::nexus_v2
