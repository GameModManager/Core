/*
 * libcbb implementation TU - single-header stb-style library. Exactly one
 * translation unit in Core defines CBB_IMPLEMENTATION before including the
 * header so the inline definitions get compiled. The header itself is
 * vendored under third_party/libcbb/libcbb.h (mirrors the pugixml layout
 * in third_party/pugixml/ - a single-header library lives there, cross-repo
 * submodules live under external/).
 */

#define CBB_IMPLEMENTATION
#include "libcbb.h"