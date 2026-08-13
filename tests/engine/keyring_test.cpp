// FileKeyring + NexusAuth keyring fallback/migration tests (Qt-free).
#include "engine/keyring.h"
#include "engine/nexus_auth.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;


#define CHECK(cond, msg)                                               \
    do {                                                               \
        INFO(msg);                                                     \
        REQUIRE(cond);                                                 \
    } while (0)

static fs::path temp_dir(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("gmm_keyring_test_" + std::string(tag) + "_" +
                  std::to_string(getpid()));
    fs::create_directories(p);
    return p;
}

TEST_CASE("keyring", "[engine]") {
    // NexusAuth's config_dir() follows XDG_CONFIG_HOME; redirect it to a temp
    // dir BEFORE the singleton is first touched so nothing writes to the real
    // user config.
    fs::path config = temp_dir("config");
    setenv("XDG_CONFIG_HOME", config.c_str(), 1);

    // ---- FileKeyring roundtrip ------------------------------------
    {
        fs::path dir = temp_dir("file");
        engine::FileKeyring kr(dir);
        const std::string name = "nexus-api-key";
        CHECK(!kr.has(name), "empty keyring has no key");
        CHECK(kr.get(name).empty(), "empty keyring get is empty");
        CHECK(kr.set(name, "secret-value-123"), "set succeeds");
        CHECK(kr.has(name), "key present after set");
        CHECK(kr.get(name) == "secret-value-123", "get returns stored value");
        CHECK(kr.set(name, "updated-value"), "re-set succeeds");
        CHECK(kr.get(name) == "updated-value", "get returns updated value");
        kr.remove(name);
        CHECK(!kr.has(name), "key gone after remove");
        CHECK(!fs::exists(dir / "keyring_nexus_api_key.dat"), "file removed");
    }

    // ---- Legacy nexus_auth.dat format compatibility ---------------
    {
        fs::path dir = temp_dir("legacy");
        engine::FileKeyring writer(dir);
        // set() writes the same XOR+b64 format the legacy file used, just
        // under a different filename — reuse it to produce a legacy file.
        CHECK(writer.set("seed", "legacy-secret"), "seed write");
        fs::copy_file(dir / "keyring_seed.dat", dir / "nexus_auth.dat",
                      fs::copy_options::overwrite_existing);
        fs::remove(dir / "keyring_seed.dat");

        CHECK(engine::FileKeyring::read_legacy(dir) == "legacy-secret",
              "legacy file decrypts to original value");
        engine::FileKeyring::remove_legacy(dir);
        CHECK(!fs::exists(dir / "nexus_auth.dat"), "legacy file removed");
    }

    // ---- NexusAuth with injected keyring (no OS keyring) ----------
    {
        fs::path primary_dir = temp_dir("primary");
        engine::NexusAuth::instance().set_keyring(
            std::make_unique<engine::FileKeyring>(primary_dir));

        auto& auth = engine::NexusAuth::instance();
        CHECK(!auth.has_api_key(), "no key initially");
        auth.set_api_key("injected-key-abc");
        CHECK(auth.has_api_key(), "key present after set");
        CHECK(auth.get_api_key() == "injected-key-abc", "get returns key");
        auth.clear_api_key();
        CHECK(!auth.has_api_key(), "key gone after clear");
    }

    // ---- NexusAuth fallback to internal file storage ---------------
    {
        auto& auth = engine::NexusAuth::instance();
        auth.set_keyring(nullptr);  // force the file fallback path
        auth.set_api_key("fallback-key-xyz");
        CHECK(auth.has_api_key(), "fallback has key");
        CHECK(auth.get_api_key() == "fallback-key-xyz", "fallback get returns key");
        CHECK(fs::exists(config / "GameModManager" / "keyring_nexus_api_key.dat"),
              "fallback wrote its file");
        auth.clear_api_key();
        CHECK(!auth.has_api_key(), "fallback cleared");
    }

    // ---- Legacy migration into the injected keyring ----------------
    {
        fs::path primary_dir = temp_dir("migrate");
        // Produce a legacy nexus_auth.dat in XDG_CONFIG_HOME's GameModManager.
        fs::path gmm_dir = config / "GameModManager";
        fs::create_directories(gmm_dir);
        engine::FileKeyring writer(gmm_dir);
        CHECK(writer.set("seed", "migrating-key-777"), "seed write");
        fs::copy_file(gmm_dir / "keyring_seed.dat", gmm_dir / "nexus_auth.dat",
                      fs::copy_options::overwrite_existing);
        fs::remove(gmm_dir / "keyring_seed.dat");

        engine::NexusAuth::instance().set_keyring(
            std::make_unique<engine::FileKeyring>(primary_dir));
        auto& auth = engine::NexusAuth::instance();
        CHECK(auth.get_api_key() == "migrating-key-777",
              "legacy key migrated into keyring");
        CHECK(!fs::exists(gmm_dir / "nexus_auth.dat"),
              "legacy file removed after migration");
        CHECK(fs::exists(primary_dir / "keyring_nexus_api_key.dat"),
              "migrated key stored in keyring");
    }

}
