#include "ui/modinfo/bbcode.h"

#include "engine/parallel/parallel.h"
#include "libcbb.h"
#include "ui/modinfo/description_browser.h"

#include <QByteArray>
#include <QChar>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QThreadPool>

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
//
// QRegularExpression is reentrant but NOT thread-safe to share across
// concurrent s.replace() calls - the implicit-shared private match state
// races. bbcode_to_html() runs on QThreadPool workers when dispatched from
// set_bbcode_html_async(), so the regex is constructed per-call. Cheap to
// build (single literal pattern) and stays inside the function's stack.
void br_tags_to_newlines(QString &s) {
  const QRegularExpression kBr(QStringLiteral("<\\s*br\\s*/?\\s*>"),
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

namespace {

// Length threshold below which bbcode_to_html + QTextBrowser layout cost
// less than the QThreadPool dispatch + queued-invoke round-trip. 1 KB
// catches tiny placeholder snippets + single-paragraph mods; everything
// longer goes through the async path. Nexus / Steam descriptions are
// typically 5-50 KB so they always land on the async side.
constexpr int kSyncThresholdBytes = 1024;

QString wrap_html(const QString &body) {
  return QStringLiteral("<html><body style=\"font-family:sans-serif; "
                        "white-space:pre-wrap;\">"
                        "%1</body></html>")
      .arg(body);
}

QString empty_placeholder_html() {
  return QStringLiteral(
      "<div style=\"text-align:center; color:grey; padding-top:24px;\">"
      "<p>No description available for this mod.</p></div>");
}

} // namespace

void set_bbcode_html_async(DescriptionBrowser *browser, const QString &desc,
                           std::atomic<unsigned> *request_token) {
  if (browser == nullptr)
    return;
  // Drop any in-flight image fetches / cached resources from the previous
  // render synchronously so stale pictures cannot survive into the new
  // document. The async HTML install lands later via the queued invoke;
  // clear_image_cache is itself synchronous so any subsequent loadResource
  // during the new layout sees an empty cache.
  browser->clear_image_cache();
  if (desc.isEmpty()) {
    browser->setHtml(empty_placeholder_html());
    return;
  }

  // Fast path: short descriptions parse + lay out faster than the
  // thread-pool dispatch + queued invoke round-trip takes. The async path
  // is only a win when bbcode_to_html costs more than ~1-2 ms.
  //
  // Multi-core toggle gate: when the user has disabled multi-core
  // processing (Settings > Performance > Enable multi-core processing),
  // run synchronously on the UI thread exactly like the pre-async
  // implementation. Bypassing the thread pool here guarantees "Off" means
  // today's single-core behavior for debugging - no dispatch, no queued
  // invoke, no QPointer race window, no token book-keeping.
  if (desc.size() < kSyncThresholdBytes || !engine::parallel::enabled()) {
    browser->setHtml(wrap_html(bbcode_to_html(desc)));
    return;
  }

  // Snapshot the request token at dispatch time. The UI-thread callback
  // compares against the caller's current value to detect stale results
  // (user moved to another mod while we were parsing). request_token may
  // be nullptr for callers that don't care about ordering.
  const unsigned token = request_token != nullptr
                             ? request_token->load(std::memory_order_relaxed)
                             : 0u;

  // QPointer is captured by value so the worker holds a non-null guard
  // for the duration of the parse. self.data() may be null even when
  // self is non-null (object destroyed after the QPointer was captured),
  // and we re-check both before the queued invoke and inside the queued
  // lambda: the panel can be torn down between dispatch and the queued
  // event firing.
  QPointer<DescriptionBrowser> self(browser);
  const QString desc_copy = desc;
  auto run = [self, desc_copy, token, request_token]() {
    const QString html = wrap_html(bbcode_to_html(desc_copy));
    QMetaObject::invokeMethod(
        self.data(),
        [self, html, token, request_token]() {
          if (!self)
            return;
          // Stale-result check: the panel has rendered a newer description
          // since we started; drop this HTML on the floor so the user
          // doesn't see old content flicker in after the new one.
          if (request_token != nullptr &&
              token != request_token->load(std::memory_order_relaxed)) {
            return;
          }
          self->setHtml(html);
        },
        Qt::QueuedConnection);
  };
  QThreadPool::globalInstance()->start(run);
}

} // namespace ui
