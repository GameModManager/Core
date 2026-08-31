#pragma once

#include <QString>

namespace ui {

// Minimal BBCode → HTML converter for Nexus mod descriptions. Supports the
// tags Nexus actually emits: b/i/u/s, color, size, url, img, quote, code,
// list/olist/*, hr, center, spoiler, headings, sub/sup, font. Unknown tags are
// dropped, unclosed tags are closed at the end, and the output is escaped.
// Pure function - no Qt widgets involved.
QString bbcode_to_html(const QString& input);

}  // namespace ui
