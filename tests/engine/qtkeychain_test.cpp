// Live OS keyring (QtKeychain) roundtrip test.
//
// This test writes a secret under service "GameModManager" in the REAL user
// keyring and deletes it at the end. A crash, a failing REQUIRE or an abort
// mid-test leaves the entry behind, so it is opt-in: set GMM_LIVE_KEYRING=1
// to run it. Without the gate this runs in CI and on developer machines as a
// side effect of the test suite, which is not worth the risk.
#include "keyring/qtkeychain_keyring.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <cstdlib>
#include <string>

TEST_CASE("qtkeychain roundtrip", "[engine]") {
  const char *gate = std::getenv("GMM_LIVE_KEYRING");
  if (!gate || std::string(gate) != "1")
    SKIP("touches the real OS keyring - set GMM_LIVE_KEYRING=1 to run");

  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QCoreApplication app(test_argc, test_argv);

  engine::QtKeychainKeyring kr;
  if (!kr.available()) {
    SKIP("no OS keyring backend running");
  }

  const std::string name = "gmm-test-roundtrip";
  const std::string v1   = "first-secret-123";
  const std::string v2   = "replaced-secret-456";

  REQUIRE(kr.set(name, v1));
  REQUIRE(kr.has(name));
  REQUIRE(kr.get(name) == v1);
  REQUIRE(kr.set(name, v2));
  REQUIRE(kr.get(name) == v2);
  kr.remove(name);
  REQUIRE_FALSE(kr.has(name));
}