#pragma once

#include <filesystem>
#include <string>

namespace engine {

class DeploymentStrategy {
public:
    virtual ~DeploymentStrategy() = default;
    virtual bool deploy(const std::filesystem::path& source,
                        const std::filesystem::path& target) = 0;
    virtual bool remove(const std::filesystem::path& target) = 0;
};

class SymlinkStrategy : public DeploymentStrategy {
public:
    bool deploy(const std::filesystem::path& source,
                const std::filesystem::path& target) override;
    bool remove(const std::filesystem::path& target) override;
};

}  // namespace engine
