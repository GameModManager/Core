#include "engine/util/thread_priority.h"

#include "platform/platform.h"

namespace engine {

// The three OS bodies live in src/platform/ (see Platform::set_thread_low_priority
// and the free-function form in platform.h). This wrapper is the historical
// name the archive extractor calls.
void set_low_priority() noexcept {
  set_thread_low_priority();
}

}  // namespace engine
