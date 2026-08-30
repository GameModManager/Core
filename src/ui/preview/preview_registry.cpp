// Registry implementation.
//
// The class is intentionally header-only (see preview_registry.h for the
// rationale: the Qt-free engine must be able to populate it without linking the
// UI library). This translation unit exists so the registry is compiled as part
// of gmm_ui and the inline singleton instance is emitted; it only needs to pull
// in the header.

#include "ui/preview/preview_registry.h"
