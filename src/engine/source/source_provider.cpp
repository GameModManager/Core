#include "engine/source/source_provider.h"

namespace engine {

SourceRegistry& SourceRegistry::instance() {
    static SourceRegistry reg;
    return reg;
}

void SourceRegistry::register_provider(std::unique_ptr<SourceProvider> provider) {
    providers_.push_back(std::move(provider));
}

SourceProvider* SourceRegistry::provider_for(const std::string& source_type) const {
    for (const auto& p : providers_) {
        if (p->source_type() == source_type) return p.get();
    }
    return nullptr;
}

std::vector<std::string> SourceRegistry::available_sources() const {
    std::vector<std::string> out;
    for (const auto& p : providers_) {
        out.push_back(p->source_type());
    }
    return out;
}

std::vector<SourceProvider*> SourceRegistry::providers() const {
    std::vector<SourceProvider*> out;
    out.reserve(providers_.size());
    for (const auto& p : providers_) {
        out.push_back(p.get());
    }
    return out;
}

} // namespace engine
