#include "engine/source/registry.h"

namespace engine {

Source::Registry &Source::Registry::instance() {
  static Source::Registry reg;
  return reg;
}

void Source::Registry::register_provider(std::unique_ptr<Source::Interface> provider) {
  providers_.push_back(std::move(provider));
}

Source::Interface *
Source::Registry::provider_for(const std::string &source_type) const {
  for (const auto &p : providers_) {
    if (p->source_type() == source_type)
      return p.get();
  }
  return nullptr;
}

std::vector<std::string> Source::Registry::available_sources() const {
  std::vector<std::string> out;
  for (const auto &p : providers_) {
    out.push_back(p->source_type());
  }
  return out;
}

std::vector<Source::Interface *> Source::Registry::providers() const {
  std::vector<Source::Interface *> out;
  out.reserve(providers_.size());
  for (const auto &p : providers_) {
    out.push_back(p.get());
  }
  return out;
}

}  // namespace engine
