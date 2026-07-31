// Live OS keyring (QtKeychain) roundtrip test.
// Skips cleanly when no keyring backend is running.
#include "keyring/qtkeychain_keyring.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    engine::QtKeychainKeyring kr;
    if (!kr.available()) {
        std::printf("qtkeychain_test: SKIP (no OS keyring backend running)\n");
        return 0;
    }

    const std::string name = "gmm-test-roundtrip";
    const std::string v1 = "first-secret-123";
    const std::string v2 = "replaced-secret-456";

    if (!kr.set(name, v1)) {
        std::printf("FAIL: set() returned false\n");
        return 1;
    }
    if (!kr.has(name)) {
        std::printf("FAIL: has() false after set\n");
        return 1;
    }
    if (kr.get(name) != v1) {
        std::printf("FAIL: get() != stored value ('%s')\n", kr.get(name).c_str());
        return 1;
    }
    if (!kr.set(name, v2)) {
        std::printf("FAIL: second set() returned false\n");
        return 1;
    }
    if (kr.get(name) != v2) {
        std::printf("FAIL: get() != replaced value\n");
        return 1;
    }
    kr.remove(name);
    if (kr.has(name)) {
        std::printf("FAIL: has() true after remove\n");
        return 1;
    }

    std::printf("qtkeychain_test: PASS (roundtrip via QtKeychain)\n");
    return 0;
}
