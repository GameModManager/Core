#include "engine/pack/adapter.h"

#include <algorithm>
#include <cctype>

namespace engine::Pack {

namespace {

  // Lowercase a string for case-insensitive prefix/suffix matching.
  std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  }

  bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
  }

  bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

}  // namespace

SourceKind detect_source_kind(const std::string &url_or_path) {
  const std::string lowered = lower(url_or_path);
  if (starts_with(lowered, "nxm://"))
    return SourceKind::NxmApi;
  return SourceKind::File;
}

bool can_handle(const std::string &url_or_path) {
  const std::string lowered = lower(url_or_path);
  return starts_with(lowered, "nxm://") || ends_with(lowered, ".gmmpack");
}

void Registry::register_adapter(std::unique_ptr<Interface> adapter) {
  adapters_.push_back(std::move(adapter));
}

Interface *Registry::adapter_for(const std::string &url_or_path) const {
  for (const auto &a : adapters_) {
    if (a->can_handle_source(url_or_path))
      return a.get();
  }
  return nullptr;
}

std::vector<Interface *> Registry::adapters() const {
  std::vector<Interface *> out;
  out.reserve(adapters_.size());
  for (const auto &a : adapters_)
    out.push_back(a.get());
  return out;
}

}  // namespace engine::Pack
