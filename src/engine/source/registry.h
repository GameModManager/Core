#pragma once

#include "engine/source/interface.h"

#include <memory>
#include <string>
#include <vector>

namespace engine::Source {

class Registry {
public:
    static Registry& instance();
    void register_provider(std::unique_ptr<Interface> provider);
    Interface* provider_for(const std::string& source_type) const;
    std::vector<std::string> available_sources() const;
    std::vector<Interface*> providers() const;
private:
    Registry() = default;
    std::vector<std::unique_ptr<Interface>> providers_;
};

} // namespace engine::Source
