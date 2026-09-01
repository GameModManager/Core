#include "ui/modinfo/bbcode.h"

#include "libcbb.h"

#include <QByteArray>

#include <string>

namespace ui {

// Thin Qt adapter over the vendored libcbb single-header BBCode-to-HTML
// library (third_party/libcbb/libcbb.h). The heavy lifting - tokenizing,
// tag stack, security sanitization (URL scheme allowlist, CSS meta-char
// rejection), and HTML escape - lives in libcbb.c. This TU only handles
// QString <-> UTF-8 std::string bridging so the Qt-using callers keep their
// existing QString-in / QString-out signature.
//
// Memory: cbb_to_html() returns a malloc'd C string (or NULL on OOM). We
// own it from this point; cbb_free() must be called exactly once. Using
// a manual cbb_free() is the simplest correct option here - the lifetime
// is bounded to this function scope, no need for std::unique_ptr with a
// custom deleter.
QString bbcode_to_html(const QString &input) {
  if (input.isEmpty()) {
    return {};
  }
  // Use the QByteArray length, not strlen(), so any embedded NUL in the
  // UTF-8 sequence is preserved instead of silently truncating the input.
  const QByteArray bytes = input.toUtf8();
  const std::string utf8(bytes.constData(), static_cast<size_t>(bytes.size()));
  char *raw = cbb_to_html(utf8.c_str());
  if (raw == nullptr) {
    return {};
  }
  const QString html = QString::fromUtf8(raw);
  cbb_free(raw);
  return html;
}

} // namespace ui
