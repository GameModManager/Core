#include "engine/modpack/collection/manifest.h"

namespace engine::Collection {

std::string_view to_string(UpdatePolicy policy) {
  switch (policy) {
  case UpdatePolicy::Exact:
    return "exact";
  case UpdatePolicy::Prefer:
    return "prefer";
  case UpdatePolicy::Latest:
    return "latest";
  }
  return "exact";
}

UpdatePolicy parse_update_policy(std::string_view text) {
  if (text == "latest")
    return UpdatePolicy::Latest;
  if (text == "prefer")
    return UpdatePolicy::Prefer;
  return UpdatePolicy::Exact;
}

}  // namespace engine::Collection
