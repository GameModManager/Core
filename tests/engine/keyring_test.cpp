// FileKeyring + NexusAuth keyring fallback/migration tests (Qt-free).
#include "engine/core/keyring/keyring.h"
#include "engine/core/log/logger.h"
#include "engine/source/nexus_auth.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

#define CHECK_MSG(cond, msg)                                                           \
  do {                                                                                 \
    INFO(msg);                                                                         \
    REQUIRE(cond);                                                                     \
  } while (0)

static fs::path temp_dir(const char *tag) {
  fs::path p = fs::temp_directory_path() / ("gmm_keyring_test_" + std::string(tag) +
                                            "_" + std::to_string(getpid()));
  fs::create_directories(p);
  return p;
}

#ifdef __linux__
// The POSIX mode bits. std::filesystem::perms is not enough here - the test
// asserts the bits the filesystem actually stored, which is the property that
// protects the secret.
static fs::perms mode_of(const fs::path &p) {
  return fs::status(p).permissions();
}

static bool is_owner_only_file(fs::perms m) {
  // Exactly rw for the owner: no group and no other bits set at all.
  return m == (fs::perms::owner_read | fs::perms::owner_write);
}
#endif

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
    CHECK_MSG(!kr.has(name), "empty keyring has no key");
    CHECK_MSG(kr.get(name).empty(), "empty keyring get is empty");
    CHECK_MSG(kr.set(name, "secret-value-123"), "set succeeds");
    CHECK_MSG(kr.has(name), "key present after set");
    CHECK_MSG(kr.get(name) == "secret-value-123", "get returns stored value");
    CHECK_MSG(kr.set(name, "updated-value"), "re-set succeeds");
    CHECK_MSG(kr.get(name) == "updated-value", "get returns updated value");
    kr.remove(name);
    CHECK_MSG(!kr.has(name), "key gone after remove");
    CHECK_MSG(!fs::exists(dir / "keyring_nexus_api_key.dat"), "file removed");
  }

  // ---- Stored files are owner-only -------------------------------
  // The secret is recoverable by anyone who can read the file, so the mode is
  // the control that matters. Assert the stored bits, not that a chmod was
  // requested: a umask of 0077 would mask a missing chmod here, and a chmod of
  // a file still open for write would not hold either.
#ifdef __linux__
  {
    fs::path dir = temp_dir("modes");
    {
      engine::FileKeyring kr(dir);
      CHECK_MSG(kr.set("nexus-api-key", "mode-check-value"), "set for mode check");
      const fs::path blob = dir / "keyring_nexus_api_key.dat";
      CHECK_MSG(fs::exists(blob), "blob written");
      CHECK_MSG(is_owner_only_file(mode_of(blob)),
                "blob is 0600 (got " +
                    std::to_string(static_cast<unsigned>(mode_of(blob))) + ")");
      const fs::path seed = dir / ".keyring_seed";
      CHECK_MSG(fs::exists(seed), "seed written");
      CHECK_MSG(is_owner_only_file(mode_of(seed)), "seed file is 0600");
      CHECK_MSG(mode_of(dir) == fs::perms::owner_all,
                "config dir is 0700 (got " +
                    std::to_string(static_cast<unsigned>(mode_of(dir))) + ")");
    }
  }

  // ---- A blob left world-readable by an older build gets tightened --
  // set() alone would leave every existing install exposed, because its file is
  // already on disk at 0644 and nothing rewrites it. get() is the read path
  // every install goes through, so that is where the tighten has to be.
  {
    fs::path dir = temp_dir("predate");
    fs::create_directories(dir);
    const fs::path blob = dir / "keyring_nexus_api_key.dat";
    engine::FileKeyring kr(dir);
    CHECK_MSG(kr.set("nexus-api-key", "pre-existing-secret"), "seed the blob");
    fs::permissions(blob, fs::perms::owner_read | fs::perms::owner_write |
                               fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);
    CHECK_MSG(!is_owner_only_file(mode_of(blob)), "blob is world-readable first");

    CHECK_MSG(kr.get("nexus-api-key") == "pre-existing-secret",
              "read still returns the value");
    CHECK_MSG(is_owner_only_file(mode_of(blob)),
              "read tightened the world-readable blob to 0600");
  }
#endif

  // ---- Per-install seed: two installs must not share a key -------
  // Same input, same machine-id, two config dirs. Before the seed file each
  // derived the key from the machine-id alone, so one install could decrypt the
  // other's file.
  {
    fs::path a_dir = temp_dir("seed_a");
    fs::path b_dir = temp_dir("seed_b");
    engine::FileKeyring a(a_dir);
    engine::FileKeyring b(b_dir);
    CHECK_MSG(a.set("nexus-api-key", "identical-plaintext"), "install A write");
    CHECK_MSG(b.set("nexus-api-key", "identical-plaintext"), "install B write");

    const auto read_blob = [](const fs::path &d) {
      std::ifstream f(d / "keyring_nexus_api_key.dat");
      std::string s;
      f >> s;
      return s;
    };
    CHECK_MSG(read_blob(a_dir) != read_blob(b_dir),
              "identical plaintext encrypts differently per install");

    const auto read_seed = [](const fs::path &d) {
      std::ifstream f(d / ".keyring_seed");
      std::string s;
      f >> s;
      return s;
    };
    CHECK_MSG(read_seed(a_dir) != read_seed(b_dir), "seeds differ per install");
    CHECK_MSG(read_seed(a_dir).size() == 32, "seed is 16 bytes, hex-encoded");
    CHECK_MSG(a.get("nexus-api-key") == "identical-plaintext", "A reads back");
    CHECK_MSG(b.get("nexus-api-key") == "identical-plaintext", "B reads back");

    // The seed is stable across instances pointed at the same dir.
    engine::FileKeyring a2(a_dir);
    CHECK_MSG(a2.get("nexus-api-key") == "identical-plaintext",
              "a second FileKeyring on the same dir reuses the seed");
  }

  // ---- Migration: a blob written under the old machine-id key ----
  // The file that shipped before the per-install seed existed. It must still
  // decrypt, and it must end up re-encrypted under a random seed so the
  // readable machine-id stops being the key.
  {
    fs::path dir = temp_dir("migrate_seed");
    fs::create_directories(dir);
    const fs::path blob = dir / "keyring_nexus_api_key.dat";
    // Written the way the old build wrote it: encrypt_with over machine_id.
    {
      std::ofstream f(blob, std::ios::trunc);
      f << engine::FileKeyring::encrypt_with(engine::FileKeyring::legacy_seed(),
                                             "old-install-secret")
        << std::endl;
    }
    CHECK_MSG(!fs::exists(dir / ".keyring_seed"),
              "no seed file before the first read");

    engine::FileKeyring kr(dir);
    CHECK_MSG(kr.get("nexus-api-key") == "old-install-secret",
              "old blob still readable after the seed change");
    CHECK_MSG(fs::exists(dir / ".keyring_seed"), "a seed was created");
    CHECK_MSG(kr.get("nexus-api-key") == "old-install-secret",
              "re-encrypted blob reads back on the second read");

    // Re-encrypted: the machine-id key no longer opens it.
    std::ifstream f(blob);
    std::string now;
    f >> now;
    CHECK_MSG(now != engine::FileKeyring::encrypt_with(
                        engine::FileKeyring::legacy_seed(), "old-install-secret"),
              "blob re-encrypted under the per-install seed");
  }

  // ---- Legacy nexus_auth.dat format compatibility ---------------
  {
    fs::path dir = temp_dir("legacy");
    engine::FileKeyring writer(dir);
    // set() writes the same XOR+b64 format the legacy file used, just
    // under a different filename — reuse it to produce a legacy file.
    CHECK_MSG(writer.set("seed", "legacy-secret"), "seed write");
    fs::copy_file(dir / "keyring_seed.dat", dir / "nexus_auth.dat",
                  fs::copy_options::overwrite_existing);
    fs::remove(dir / "keyring_seed.dat");

    CHECK_MSG(engine::FileKeyring::read_legacy(dir) == "legacy-secret",
              "legacy file decrypts to original value");
    engine::FileKeyring::remove_legacy(dir);
    CHECK_MSG(!fs::exists(dir / "nexus_auth.dat"), "legacy file removed");
  }

  // ---- NexusAuth with injected keyring (no OS keyring) ----------
  {
    fs::path primary_dir = temp_dir("primary");
    engine::NexusAuth::instance().set_keyring(
        std::make_unique<engine::FileKeyring>(primary_dir));

    auto &auth = engine::NexusAuth::instance();
    CHECK_MSG(!auth.has_api_key(), "no key initially");
    auth.set_api_key("injected-key-abc");
    CHECK_MSG(auth.has_api_key(), "key present after set");
    CHECK_MSG(auth.get_api_key() == "injected-key-abc", "get returns key");
    auth.clear_api_key();
    CHECK_MSG(!auth.has_api_key(), "key gone after clear");
  }

  // ---- NexusAuth fallback to internal file storage ---------------
  {
    auto &auth = engine::NexusAuth::instance();
    auth.set_keyring(nullptr);  // force the file fallback path
    auth.set_api_key("fallback-key-xyz");
    CHECK_MSG(auth.has_api_key(), "fallback has key");
    CHECK_MSG(auth.get_api_key() == "fallback-key-xyz", "fallback get returns key");
    CHECK_MSG(fs::exists(config / "GameModManager" / "keyring_nexus_api_key.dat"),
              "fallback wrote its file");
    auth.clear_api_key();
    CHECK_MSG(!auth.has_api_key(), "fallback cleared");
  }

  // ---- Warn on read when no keyring backend is available ---------
  // The old check asked effective_keyring().available(), and that returns a
  // FileKeyring whose available() is unconditionally true - so the promised
  // "falling back to insecure storage" warning could never fire. Assert on the
  // message the fallback path actually produces now.
  {
    auto &auth = engine::NexusAuth::instance();
    auth.set_keyring(nullptr);
    auth.set_api_key("warn-on-read-key");
    // Static because Logger::add_callback holds its callback for the life of
    // the process - a lambda over a local vector would dangle after this block.
    static std::vector<std::string> seen;
    engine::Logger::instance().add_callback(
        [](engine::LogLevel, const std::string &, const std::string &msg) {
          seen.push_back(msg);
        });
    seen.clear();
    CHECK_MSG(auth.get_api_key() == "warn-on-read-key", "fallback read succeeds");

    const auto contains = [](const std::string &needle) {
      for (const auto &m : seen)
        if (m.find(needle) != std::string::npos)
          return true;
      return false;
    };
    // Negative control: the capture has to be live, or the check below would
    // pass on an empty vector.
    engine::Logger::instance().warn("NexusAuth: control probe");
    CHECK_MSG(contains("control probe"), "log capture is live");
    CHECK_MSG(contains("NexusAuth: no OS keyring"),
              "read with no keyring backend warns (got " +
                  std::to_string(seen.size()) + " captured lines)");
    auth.clear_api_key();
  }

  // ---- Legacy migration into the injected keyring ----------------
  {
    fs::path primary_dir = temp_dir("migrate");
    // Produce a legacy nexus_auth.dat in XDG_CONFIG_HOME's GameModManager.
    fs::path gmm_dir = config / "GameModManager";
    fs::create_directories(gmm_dir);
    engine::FileKeyring writer(gmm_dir);
    CHECK_MSG(writer.set("seed", "migrating-key-777"), "seed write");
    fs::copy_file(gmm_dir / "keyring_seed.dat", gmm_dir / "nexus_auth.dat",
                  fs::copy_options::overwrite_existing);
    fs::remove(gmm_dir / "keyring_seed.dat");

    engine::NexusAuth::instance().set_keyring(
        std::make_unique<engine::FileKeyring>(primary_dir));
    auto &auth = engine::NexusAuth::instance();
    CHECK_MSG(auth.get_api_key() == "migrating-key-777",
              "legacy key migrated into keyring");
    CHECK_MSG(!fs::exists(gmm_dir / "nexus_auth.dat"),
              "legacy file removed after migration");
    CHECK_MSG(fs::exists(primary_dir / "keyring_nexus_api_key.dat"),
              "migrated key stored in keyring");
  }
}
