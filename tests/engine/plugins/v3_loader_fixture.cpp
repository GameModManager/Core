// v3 P3 test fixture - exports gmm_abi_version -> 3 and gmm_entry -> a
// GmmPluginDesc registering 2 games + 2 capabilities. Built only for the
// loader test; never shipped.

#include "gmm_abi_v3.h"

#include <cstring>

static const uint32_t alpha_appids[] = {480};
static const uint32_t beta_appids[]  = {481};

static const char *alpha_execs[] = {"alpha.exe"};
static const char *beta_execs[]  = {"beta.exe"};

static GmmGameEntity games[] = {
    {1, "V3Alpha", "V3 Alpha", alpha_appids, 1, nullptr, nullptr, "v3alpha", nullptr, 0,
     "Data", alpha_execs, 1},
    {1, "V3Beta", "V3 Beta", beta_appids, 1, nullptr, nullptr, "v3beta", nullptr, 0,
     "Data", beta_execs, 1},
};

// Two data-only capabilities (NULL invoke).
static GmmCapabilityObject caps[] = {
    {"load_order.encoding", "V3Alpha", 1, 0, nullptr, nullptr},
    {"conflicts.scope", "V3Beta", 1, 0, nullptr, nullptr},
};

static GmmPluginDesc desc = {
    3, "V3LoaderFixture", "test", "1.0", "v3 loader fixture", games, 2, caps, 2,
};

extern "C" uint32_t gmm_abi_version(void) {
  return GMM_API_VERSION;
}

extern "C" const GmmPluginDesc *gmm_entry(const GmmHostServices *host) {
  (void)host;
  return &desc;
}
