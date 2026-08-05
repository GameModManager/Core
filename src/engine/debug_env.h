#ifndef GMM_DEBUG_ENV_H
#define GMM_DEBUG_ENV_H

#include <stdlib.h>

// GMM_DEBUG is a VALUE, not a presence flag: GMM_DEBUG=1 (or true/yes)
// enables debug output; GMM_DEBUG=0 or unset disables it. A presence-based
// check treated "GMM_DEBUG=0" as enabled, which leaked DBG lines to the
// console AND disabled the overlay stderr capture (both are gated on this).
// Lives in its own C-compatible header so the LD_PRELOAD intercept (.c) can
// share it without pulling in C++.
static inline int gmm_debug_enabled(void) {
    const char* v = getenv("GMM_DEBUG");
    if (!v) return 0;
    switch (v[0]) {
        case '1':              // 1
        case 't': case 'T':    // true / TRUE
        case 'y': case 'Y':    // yes / YES
            return 1;
        default:
            return 0;
    }
}

#endif
