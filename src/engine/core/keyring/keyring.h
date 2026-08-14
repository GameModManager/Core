#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

// Abstract OS-backed secret storage. The app layer injects a platform
// implementation (QtKeychain on desktop OSes); the engine stays Qt-free.
class Keyring {
public:
    virtual ~Keyring() = default;

    // True when the backend is reachable and should be used.
    virtual bool available() const = 0;
    virtual bool has(const std::string& name) const = 0;
    virtual std::string get(const std::string& name) const = 0;
    virtual bool set(const std::string& name, const std::string& value) = 0;
    virtual void remove(const std::string& name) = 0;
};

// Obfuscated file-backed storage (XOR + base64). NOT real crypto — a last
// resort for systems without an OS keyring. Logs a prominent warning so callers
// know the stored secret is recoverable from the binary.
class FileKeyring : public Keyring {
public:
    explicit FileKeyring(std::filesystem::path config_dir);

    bool available() const override { return true; }
    bool has(const std::string& name) const override;
    std::string get(const std::string& name) const override;
    bool set(const std::string& name, const std::string& value) override;
    void remove(const std::string& name) override;

    // Legacy pre-keyring storage (nexus_auth.dat, old XOR format). Used by
    // NexusAuth to migrate an existing key into the OS keyring exactly once.
    static std::string read_legacy(const std::filesystem::path& config_dir);
    static void remove_legacy(const std::filesystem::path& config_dir);

private:
    std::filesystem::path file_for(const std::string& name) const;

    static std::string derive_key();
    static std::string encrypt(const std::string& plaintext);
    static std::string decrypt(const std::string& ciphertext);
    static std::string base64_encode(const std::string& in);
    static std::string base64_decode(const std::string& in);
    static std::string machine_id();

    std::filesystem::path config_dir_;
};

} // namespace engine
