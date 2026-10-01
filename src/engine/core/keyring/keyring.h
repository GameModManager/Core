#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine {

// Abstract OS-backed secret storage. The app layer injects a platform
// implementation (QtKeychain on desktop OSes); the engine stays Qt-free.
class Keyring {
public:
  virtual ~Keyring() = default;

  // True when the backend is reachable and should be used.
  virtual bool available() const                                      = 0;
  virtual bool has(const std::string &name) const                     = 0;
  virtual std::string get(const std::string &name) const              = 0;
  virtual bool set(const std::string &name, const std::string &value) = 0;
  virtual void remove(const std::string &name)                        = 0;
};

// Obfuscated file-backed storage (XOR + base64). NOT real crypto - a last
// resort for systems without an OS keyring. Logs a prominent warning so callers
// know the stored secret is recoverable from the binary.
//
// Files are owner-only (0600) and the directory is owner-only (0700), so the
// obfuscation is the only thing between a local user and the secret - which is
// also why the key is a per-install random seed rather than anything another
// local user can read (machine-id is world-readable on Linux).
class FileKeyring : public Keyring {
public:
  explicit FileKeyring(std::filesystem::path config_dir);

  bool available() const override { return true; }
  bool has(const std::string &name) const override;
  std::string get(const std::string &name) const override;
  bool set(const std::string &name, const std::string &value) override;
  void remove(const std::string &name) override;

  // Legacy pre-keyring storage (nexus_auth.dat, old XOR format). Used by
  // NexusAuth to migrate an existing key into the OS keyring exactly once.
  static std::string read_legacy(const std::filesystem::path &config_dir);
  static void remove_legacy(const std::filesystem::path &config_dir);

  // XOR key material. Explicit-seed so a blob written by a build that predates
  // the per-install seed file can be decrypted and re-encrypted under the new
  // one; the migration regression test drives the same primitives the old
  // build used rather than a second copy of the algorithm.
  static std::string encrypt_with(const std::string &seed,
                                  const std::string &plaintext);
  static std::string decrypt_with(const std::string &seed,
                                  const std::string &ciphertext);

  // The seed a build that predates keyring_seed.dat derived the key from:
  // machine_id() when readable, else the constant that build fell back to.
  // Only ever used to read an existing blob, never to create one.
  static std::string legacy_seed();

  // Every key a build that predates the per-install seed could have written a
  // blob under. A blob's key is its own property, and the write-time condition
  // is not recoverable from the file, so a read has to try each of these.
  static std::vector<std::string> legacy_seeds();

private:
  std::filesystem::path file_for(const std::string &name) const;

  // Decrypts under `seed` and returns the plaintext only if it is a value this
  // keyring could have stored. A wrong key does not fail - it yields a
  // same-length string of near-random bytes - so callers must never treat
  // "non-empty" as "decrypted".
  static std::string decrypt_if_valid(const std::string &seed,
                                      const std::string &ciphertext);

  // The 16-byte random seed in <config_dir>/.keyring_seed, created on
  // first use. Static so the legacy blob reader (which has a config dir but no
  // instance) resolves the seed the same way.
  static std::string load_or_create_seed(const std::filesystem::path &dir);
  static bool write_seed(const std::filesystem::path &dir,
                         const std::string &seed);

  // Writes and closes the blob, then tightens it - a still-open fd would be
  // re-created with the wider mode.
  static bool write_blob(const std::filesystem::path &path,
                         const std::string &ciphertext);

  static std::string derive_key(const std::string &seed);
  static std::string base64_encode(const std::string &in);
  static std::string base64_decode(const std::string &in);
  static std::string machine_id();

  // Owner-only (0600) on a file that already exists - a blob written by an
  // older build is still world-readable until it is read back.
  static void restrict(const std::filesystem::path &path);

  std::filesystem::path config_dir_;
};

}  // namespace engine
