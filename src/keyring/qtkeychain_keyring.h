#pragma once

#include "engine/keyring.h"

#include <QObject>

#include <string>

namespace engine {

// OS-backed secret storage via QtKeychain. QtKeychain hides the platform
// behind one async job API (Secret Service on Linux, KWallet when it wins
// detection, Keychain on macOS, Credential Locker on Windows) and
// auto-detects the available backend. Jobs must run on a thread with a Qt
// event loop, so the Keyring interface stays synchronous by running each job
// on the Qt main thread and blocking until it reports.
class QtKeychainKeyring final : public QObject, public Keyring {
public:
    QtKeychainKeyring();
    ~QtKeychainKeyring() override;

    bool available() const override;
    bool has(const std::string& name) const override;
    std::string get(const std::string& name) const override;
    bool set(const std::string& name, const std::string& value) override;
    void remove(const std::string& name) override;

private:
    bool do_available() const;
    bool do_has(const std::string& name) const;
    std::string do_get(const std::string& name) const;
    bool do_set(const std::string& name, const std::string& value);
    void do_remove(const std::string& name) const;

    // Availability is probed once (a backend does not appear mid-session on
    // desktop Linux) and cached to avoid a job roundtrip on every access.
    mutable bool available_cached_ = false;
    mutable bool available_checked_ = false;
};

} // namespace engine
