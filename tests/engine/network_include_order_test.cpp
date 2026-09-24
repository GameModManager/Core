// Compile-order guard for the CURL forward-declaration shim in
// engine/network/network_manager.h (Workspace-3cit).
//
// The shim header is deliberately included BEFORE <curl/curl.h> here - the
// order that used to fail with a struct-vs-void typedef conflict once the
// CURLINC_CURL_H guard no longer skipped the shim. If this TU compiles, the
// shim spelling matches the installed libcurl regardless of include order.
#include "engine/network/network_manager.h"

#include <curl/curl.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

static_assert(std::is_same_v<CURL, void>,
              "shim spelling must match curl.h (typedef void CURL)");
static_assert(std::is_same_v<CURLSH, void>,
              "shim spelling must match curl.h (typedef void CURLSH)");

TEST_CASE("network shim: header compiles before curl.h", "[engine][network]") {
  // Compilation itself is the assertion; touch one shim-adjacent type so the
  // TU is not include-only.
  const engine::network::NetworkOptions opts;
  REQUIRE(opts.default_timeout_seconds == 30);
}
