#pragma once

#include <QString>

namespace ui {

// Thin Qt adapter over the vendored libcbb single-header BBCode-to-HTML
// library (third_party/libcbb/libcbb.h). Converts QString <-> std::string
// around cbb_to_html() and frees the malloc'd C string with cbb_free().
//
// Note: libcbb rejects javascript:/data: URLs (security hardening over the
// old in-house bbcode.cpp converter) and strips CSS meta-characters from
// [color]/[size]/[font] arguments. [code] and [noparse] both imply noparse
// semantics - inner tags are not processed.
QString bbcode_to_html(const QString &input);

} // namespace ui
