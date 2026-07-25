#pragma once

#include <filesystem>
#include <string>

namespace engine {

class Runtime {
public:
    virtual ~Runtime() = default;
    virtual bool launch(const std::filesystem::path& executable,
                        const std::string& game_id) = 0;
    virtual bool is_available() const = 0;
    virtual std::string name() const = 0;
};

class NativeRuntime : public Runtime {
public:
    bool launch(const std::filesystem::path& executable,
                const std::string& game_id) override;
    bool is_available() const override;
    std::string name() const override { return "native"; }
};

class ProtonRuntime : public Runtime {
public:
    bool launch(const std::filesystem::path& executable,
                const std::string& game_id) override;
    bool is_available() const override;
    std::string name() const override { return "proton"; }

private:
    std::filesystem::path find_proton() const;
};

}  // namespace engine
