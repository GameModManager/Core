// v3 P3 test fixture - declares ABI version 99 (unknown) to exercise the
// clean-refusal path. Exports gmm_entry too, but the loader must refuse
// before calling it (version symbol is authoritative).

#include "gmm_abi_v3.h"

static GmmPluginDesc desc = {3,       "Mismatch", "test",  "1.0", "mismatch",
                             nullptr, 0,          nullptr, 0};

extern "C" uint32_t gmm_abi_version(void) {
  return 99u;
}

extern "C" const GmmPluginDesc *gmm_entry(const GmmHostServices *host) {
  (void)host;
  return &desc;
}
