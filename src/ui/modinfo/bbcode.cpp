#include "ui/modinfo/bbcode.h"

#include "libcbb.h"

#include <QByteArray>
#include <QChar>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <string>

namespace ui {

namespace {

// Decode a single named or numeric HTML entity to a QChar. Returns a null
// QChar (isNull() == true) when the substring is not a known entity, so the
// caller can leave the original bytes untouched.
//
// Supports the small set actually emitted by the Nexus / Steam BBCode
// dialects: &amp; &lt; &gt; &quot; &apos; &nbsp; plus the numeric forms
// &#NNN; and &#xHH;. Unknown / malformed entities pass through as-is so
// the user sees the literal "&xyz;" instead of silently swallowing
// characters.
QChar decode_entity(const QString &s, int start, int *consumed) {
  // s[start] is '&'. Need a closing ';' within a reasonable window.
  constexpr int kMaxLen = 8; // covers &#xHHHH; (longest possible)
  int semi = -1;
  for (int i = start + 1; i < s.size() && i - start <= kMaxLen; ++i) {
    if (s[i] == QLatin1Char(';')) {
      semi = i;
      break;
    }
  }
  if (semi < 0) {
    *consumed = 0;
    return QChar();
  }
  const int body_len = semi - (start + 1);
  if (body_len <= 0) {
    *consumed = 0;
    return QChar();
  }
  const QStringView body = QStringView(s).mid(start + 1, body_len);
  QChar out;
  // Numeric: &#NNN; or &#xHH;
  if (body_len > 1 && body[0] == QLatin1Char('#')) {
    bool ok = false;
    ushort cp;
    if (body[1] == QLatin1Char('x') || body[1] == QLatin1Char('X')) {
      // Hex
      const QString hex_part = s.mid(start + 3, body_len - 2);
      cp = hex_part.toUShort(&ok, 16);
    } else {
      const QString dec_part = s.mid(start + 2, body_len - 1);
      cp = dec_part.toUShort(&ok, 10);
    }
    if (ok) {
      out = QChar(cp);
    }
  } else {
    // Named entities (case-sensitive per HTML5). The five mark-up
    // entities plus the common &nbsp;.
    if (body == QLatin1String("amp")) {
      out = QLatin1Char('&');
    } else if (body == QLatin1String("lt")) {
      out = QLatin1Char('<');
    } else if (body == QLatin1String("gt")) {
      out = QLatin1Char('>');
    } else if (body == QLatin1String("quot")) {
      out = QLatin1Char('"');
    } else if (body == QLatin1String("apos")) {
      out = QLatin1Char('\'');
    } else if (body == QLatin1String("nbsp")) {
      out = QLatin1Char(' ');
    }
    // Everything else (e.g. &copy;, &trade;) is left as-is.
  }
  if (out.isNull()) {
    *consumed = 0;
    return QChar();
  }
  *consumed = (semi - start) + 1; // include the ';'
  return out;
}

// Single-pass, non-recursive HTML-entity unescape. The Nexus / Steam APIs
// pre-escape their BBCode text (e.g. "Gameplay &gt; Controller" to render
// the literal ">" character). libcbb is a BBCode parser; it re-escapes the
// '&' inside the entity, so "&gt;" round-trips to "&amp;gt;" and
// QTextBrowser displays the literal "&gt;". Unescaping BEFORE libcbb fixes
// that, and libcbb will re-escape the resulting real character back into
// the correct single-escaped form for the browser.
//
// Single pass on purpose: re-scanning the output would collapse
// "&amp;amp;" into "&amp;" (losing the user's intent for a literal
// "&amp;"). The API's double-escape is a vanishingly rare edge case and
// not worth the surprise.
QString unescape_html_entities(const QString &s) {
  QString out;
  out.reserve(s.size());
  for (int i = 0; i < s.size();) {
    if (s[i] != QLatin1Char('&')) {
      out.append(s[i]);
      ++i;
      continue;
    }
    int consumed = 0;
    const QChar ch = decode_entity(s, i, &consumed);
    if (ch.isNull()) {
      // Not an entity - emit the '&' verbatim, advance one.
      out.append(s[i]);
      ++i;
      continue;
    }
    out.append(ch);
    i += consumed;
  }
  return out;
}

// The Nexus API sprinkles raw HTML <br /> tags inside BBCode descriptions
// (likely because the edit box is HTML-flavored). libcbb has no concept
// of <br> - it falls through as text and gets escaped to "&lt;br /&gt;",
// which QTextBrowser then displays literally. Convert every variant to
// '\n' BEFORE libcbb; libcbb preserves raw newlines, and the panel wraps
// the body in white-space:pre-wrap, so '\n' becomes a visible line break.
//
// Case-insensitive on the tag name, flexible on the trailing slash and
// whitespace. Matches: <br>, <br/>, <br />, <BR>, <Br   />.
void br_tags_to_newlines(QString &s) {
  static const QRegularExpression kBr(
      QStringLiteral("<\\s*br\\s*/?\\s*>"),
      QRegularExpression::CaseInsensitiveOption);
  s.replace(kBr, QStringLiteral("\n"));
}

} // namespace

// Thin Qt adapter over the vendored libcbb single-header BBCode-to-HTML
// library (third_party/libcbb/libcbb.h). The heavy lifting - tokenizing,
// tag stack, security sanitization (URL scheme allowlist, CSS meta-char
// rejection), and HTML escape - lives in libcbb.c. This TU only handles
// QString <-> UTF-8 std::string bridging, plus the input normalization
// the Nexus / Steam BBCode dialects need (entity unescape, <br> -> \n).
//
// Normalization order matters: unescape entities FIRST, so an "&amp;lt;"
// from the API round-trips to "<" via "&lt;" -> "<" (the second pass
// inside libcbb then re-escapes the '<' to "&lt;" which renders as the
// literal "<" character). The "br-to-newline" pass happens after so it
// sees the literal "<br />" the API emitted, not a pre-unescaped tag.
QString bbcode_to_html(const QString &input) {
  if (input.isEmpty()) {
    return {};
  }
  QString normalized = unescape_html_entities(input);
  br_tags_to_newlines(normalized);
  // Use the QByteArray length, not strlen(), so any embedded NUL in the
  // UTF-8 sequence is preserved instead of silently truncating the input.
  const QByteArray bytes = normalized.toUtf8();
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
