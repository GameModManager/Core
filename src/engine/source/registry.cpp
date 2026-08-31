#include "engine/source/registry.h"

namespace engine::Source {

Registry& Registry::instance() {
    static Registry reg;
    return reg;
}

void Registry::register_provider(std::unique_ptr<Interface> provider) {
    providers_.push_back(std::move(provider));
}

Interface* Registry::provider_for(const std::string& source_type) const {
    for (const auto& p : providers_) {
        if (p->source_type() == source_type) return p.get();
    }
    return nullptr;
}

std::vector<std::string> Registry::available_sources() const {
    std::vector<std::string> out;
    for (const auto& p : providers_) {
        out.push_back(p->source_type());
    }
    return out;
}

std::vector<Interface*> Registry::providers() const {
    std::vector<Interface*> out;
    out.reserve(providers_.size());
    for (const auto& p : providers_) {
        out.push_back(p.get());
    }
    return out;
}

} // namespace engine::Source
