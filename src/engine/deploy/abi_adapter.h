#pragma once

#include "engine/deploy/interface.h"

#include <string>
#include <vector>

namespace Deploy {

// Adapter that wraps v2 plugin's GmmDeployFnV2 / GmmRemoveFnV2 callbacks into
// the engine's Interface. When a plugin registers a deploy strategy, the
// pipeline uses this adapter instead of the default Symlink / OverlayFsDeploy.
class AbiAdapter : public Interface {
public:
  using DeployFn = int (*)(const char *, const char *, void *);
  using RemoveFn = int (*)(const char *, void *);

  AbiAdapter(DeployFn deploy_fn, RemoveFn remove_fn, void *user_data)
      : deploy_fn_(deploy_fn), remove_fn_(remove_fn), user_data_(user_data) {}

  bool deploy(const std::filesystem::path &source,
              const std::filesystem::path &target) override {
    if (!deploy_fn_)
      return false;

    const int result = deploy_fn_(source.string().c_str(),
                                  target.string().c_str(), user_data_);
    return result != 0;
  }

  bool remove(const std::filesystem::path &target) override {
    if (!remove_fn_)
      return false;

    const int result = remove_fn_(target.string().c_str(), user_data_);
    return result != 0;
  }

private:
  DeployFn deploy_fn_;
  RemoveFn remove_fn_;
  void *user_data_;
};

} // namespace Deploy
