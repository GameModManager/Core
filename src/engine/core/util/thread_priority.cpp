#include "engine/core/util/thread_priority.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace engine {

void set_low_priority() noexcept {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    // nice value 10 = below normal. Lowering (raising the nice value) is always
    // permitted for an unprivileged process; ignore any error (best-effort).
    (void)setpriority(PRIO_PROCESS, 0, 10);
#endif
}

}  // namespace engine
