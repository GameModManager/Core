// Live OS keyring (QtKeychain) roundtrip test.
// Skips cleanly when no keyring backend is running.
#include "keyring/qtkeychain_keyring.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>

#include <string>

TEST_CASE("qtkeychain roundtrip", "[engine]") {
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QCoreApplication app(test_argc, test_argv);

    engine::QtKeychainKeyring kr;
    if (!kr.available()) {
        SKIP("no OS keyring backend running");
    }

    const std::string name = "gmm-test-roundtrip";
    const std::string v1 = "first-secret-123";
    const std::string v2 = "replaced-secret-456";

    REQUIRE(kr.set(name, v1));
    REQUIRE(kr.has(name));
    REQUIRE(kr.get(name) == v1);
    REQUIRE(kr.set(name, v2));
    REQUIRE(kr.get(name) == v2);
    kr.remove(name);
    REQUIRE_FALSE(kr.has(name));
}