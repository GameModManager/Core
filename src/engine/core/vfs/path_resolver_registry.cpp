#include "engine/core/vfs/path_resolver_registry.h"

#include "engine/core/events/event_bus.h"

namespace engine::vfs {

PathResolverRegistry &PathResolverRegistry::instance() {
  static PathResolverRegistry reg;
  return reg;
}

PathResolver &PathResolverRegistry::resolver(const std::filesystem::path &root,
                                             NameCompare cmp) {
  const std::string key =
      root.string() + "|" + (cmp == NameCompare::CaseInsensitive ? "ci" : "cs");
  auto it = resolvers_.find(key);
  if (it != resolvers_.end())
    return it->second;
  auto [ins, _] = resolvers_.emplace(key, PathResolver(root, cmp));
  return ins->second;
}

void PathResolverRegistry::invalidate_all() {
  for (auto &[key, r] : resolvers_)
    r.invalidate_all();
}

PathResolverRegistry::PathResolverRegistry() {
  // Any change to the merged mod view invalidates every cached resolver index:
  // a mod enable/disable, reorder, install, removal, or a profile switch all
  // change which file wins a given path, so the derived caches must be dropped.
  // PathResolver's index is a cache, not the authority - the deploy ledger and
  // the on-disk tree remain the source of truth, so a stale cache can only
  // cost a re-scan, never a wrong deploy.
  auto handler = [](const std::string &, const std::string &) {
    instance().invalidate_all();
  };
  static const char *const kInvalidatingEvents[] = {
      engine::events::kModStateChanged, engine::events::kModMoved,
      engine::events::kProfileChanged,  engine::events::kModInstalled,
      engine::events::kModRemoved,
  };
  for (const char *ev : kInvalidatingEvents)
    subs_.push_back(engine::EventBus::instance().subscribe(ev, handler));
}

} // namespace engine::vfs
