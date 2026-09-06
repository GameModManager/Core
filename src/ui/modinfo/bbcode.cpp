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

#include <algorithm>
#include <array>
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

// Normalize line endings to '\n'. Raw BBCode and HTML frequently arrive
// with '\r\n' (Nexus, Steam) or even bare '\r' (some old Mac-era exports).
// The br-tag dedup and newline-collapse passes below only make sense on a
// single canonical form. QRegularExpression is reentrant but NOT thread-
// safe to share across concurrent s.replace() calls - the implicit-shared
// private match state races. bbcode_to_html() runs on QThreadPool workers
// when dispatched from set_bbcode_html_async(), so the regex is built
// per-call. Cheap.
void normalize_line_endings(QString &s) {
  const QRegularExpression kCr(QStringLiteral("\r\n?"));
  s.replace(kCr, QStringLiteral("\n"));
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
// Br adjacency dedup: when the source has "<br>\n" or "\n<br>" the two
// line breaks double up under pre-wrap. We delete ONE adjacent '\n' so
// the visible break count matches what the page author intended (a
// single <br> is a soft break, not a paragraph).
//
// The regex is built per-call (see normalize_line_endings for why).
void br_tags_to_newlines(QString &s) {
  // Consume an optional trailing newline after the <br>. Example:
  // "a<br>\nb" -> "a\nb" (one soft break, not a paragraph).
  const QRegularExpression kBrTrail(
      QStringLiteral("<\\s*br\\s*/?\\s*>\n"),
      QRegularExpression::CaseInsensitiveOption);
  s.replace(kBrTrail, QStringLiteral("\n"));
  // Consume an optional leading newline before the <br>. Example:
  // "a\n<br>b" -> "a\nb". Run AFTER the trail pass so any "<br>\n"
  // that already became "\n" does not accidentally match the leading
  // pattern's "\n" + "<br>" shape.
  const QRegularExpression kBrLead(
      QStringLiteral("\n<\\s*br\\s*/?\\s*>"),
      QRegularExpression::CaseInsensitiveOption);
  s.replace(kBrLead, QStringLiteral("\n"));
  // A lone <br> with no adjacent newline still gets converted.
  const QRegularExpression kBrAlone(
      QStringLiteral("<\\s*br\\s*/?\\s*>"),
      QRegularExpression::CaseInsensitiveOption);
  s.replace(kBrAlone, QStringLiteral("\n"));
}

// Collapse runs of 3+ blank lines down to a single paragraph break
// ("\n\n") and trim leading / trailing newlines. The pre-wrap wrapper
// renders every preserved '\n' as a visible break; we want at most one
// blank line between paragraphs, matching what a browser shows for
// `<p>A</p><p>B</p>`.
//
// List-block pass: inside a [list]...[/list] span, <li> already
// carries its own visual margin in QTextBrowser, so any '\n{2,}'
// immediately before the next `[*]` / `[li]` (or before `[/list]`) is
// pure inflation. The Nexus / LL permissions list example arrives as
// `[*]item1\r\n\r\n[*]item2\r\n\r\n[*]item3` with the author using a
// separate paragraph for each item; without this pass every item gets
// a visible blank line on top of the <li> margin. The case-insensitive
// flag covers [LIST] / [LI] variants. The lookahead keeps the marker
// itself outside the match so we only consume the newline run.
void collapse_blank_lines(QString &s) {
  // "\n{3,}" -> "\n\n"
  const QRegularExpression kThreePlus(QStringLiteral("\n{3,}"));
  s.replace(kThreePlus, QStringLiteral("\n\n"));
  // Inside list blocks: collapse \n{2,} immediately before a list-item
  // marker or a list-close. Lookahead keeps the marker untouched and
  // lets us replace the run with a single '\n'.
  const QRegularExpression kListTighten(
      QStringLiteral(R"(\n{2,}(?=\[\*\]|\[li\]|\[/li\]|\[/list\]))"),
      QRegularExpression::CaseInsensitiveOption);
  s.replace(kListTighten, QStringLiteral("\n"));
  // Strip leading and trailing whitespace; cheap O(n) scan and avoids
  // rendering a stray blank line before / after the description.
  int start = 0;
  while (start < s.size() && s[start].isSpace())
    ++start;
  int end = s.size();
  while (end > start && s[end - 1].isSpace())
    --end;
  if (start == 0 && end == s.size())
    return;
  s = s.mid(start, end - start);
}

// Lightweight URL scheme allowlist. Mirrors what libcbb's cbb_url_ok()
// accepts (http / https / mailto / ftp / ftps). Used to gate HTML-anchor
// and bare-URL linkification so javascript:, data:, vbscript:, etc. never
// reach libcbb as live hrefs. Anchor hrefs and bare URLs that fail this
// check are left as plain text (libcbb will then re-escape the '&' etc.).
constexpr std::array<QStringView, 5> kAllowedSchemes = {
    QStringView(u"http://"), QStringView(u"https://"),
    QStringView(u"mailto:"), QStringView(u"ftp://"),
    QStringView(u"ftps://")};

bool is_url_scheme_ok(const QString &url) {
  if (url.isEmpty())
    return false;
  // Case-insensitive scheme prefix. Whitelist: http(s), mailto, ftp(s).
  const auto lower = url.toLower();
  return std::any_of(
      kAllowedSchemes.begin(), kAllowedSchemes.end(),
      [&lower](QStringView prefix) { return lower.startsWith(prefix); });
}

// Convert raw HTML <a href="...">text</a> anchors into BBCode
// [url=...]text[/url] so libcbb can parse them as links. The LoversLab /
// Mod.pub scrapes deliver rich HTML where the user-visible description
// (e.g. <div class="ipsType_richText">) carries <a href=...>anchors. Without
// this pass libcbb treats the < and > as text and the links are lost.
//
// We allow only the same schemes libcbb itself accepts (see
// is_url_scheme_ok). Disallowed hrefs (javascript:, relative paths, etc.)
// are dropped to "[url]{text}[/url]"-less plain text by skipping the
// conversion entirely (the surrounding < and > are then plain text and
// get escaped by libcbb). Hrefs without an explicit scheme but starting
// with "/" or "#" are also dropped - those are page-internal links that
// would 404 in a downloaded description.
//
// The text inside the anchor is preserved as-is (it may already contain
// BBCode - libcbb will still parse the inner tags). Whitespace inside
// the tag is tolerated to handle the wild HTML editors emit.
void convert_html_anchors_to_bbcode(QString &s) {
  // Two passes: simple <a href="...">text</a>, then single-quoted form.
  // The regex is intentionally not greedy on the body so the first
  // closing </a> matches. We iterate with a moving search offset to
  // avoid the implicit-shared state race (see normalize_line_endings)
  // and to guarantee termination on degenerate inputs.
  auto run_pass = [&](const QChar quote) {
    const QString pattern =
        QStringLiteral("<a\\b[^>]*\\bhref\\s*=\\s*%1([^\"%1]*)%1[^>]*>(["
                       "\\s\\S]*?)</a>")
            .arg(quote);
    QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
    int search_from = 0;
    while (search_from < s.size()) {
      QRegularExpressionMatch m = re.match(s, search_from);
      if (!m.hasMatch())
        break;
      const QString href = m.captured(1);
      const QString text = m.captured(2);
      QString replacement;
      if (is_url_scheme_ok(href)) {
        replacement = QStringLiteral("[url=%1]%2[/url]").arg(href, text);
      } else {
        // Drop the anchor; keep just the visible text. (Pre-existing
        // BBCode inside the text is preserved verbatim for libcbb.)
        replacement = text;
      }
      s.replace(m.capturedStart(), m.capturedLength(), replacement);
      // Step past the replaced span (which can be longer or shorter
      // than the original). The next regex match starts at the old
      // end so we always make forward progress even when replacement
      // is empty.
      search_from = m.capturedEnd();
    }
  };
  run_pass(QLatin1Char('"'));
  run_pass(QLatin1Char('\''));
}

// Auto-linkify bare https?:// URLs that are not already inside a BBCode
// [url] tag or an HTML anchor. Run AFTER convert_html_anchors_to_bbcode
// (so the anchors are already converted) and AFTER br_tags_to_newlines
// (so URLs that span a line break - rare but happens on copy/paste - are
// still captured). The check `is_url_scheme_ok` keeps the allowlist
// narrow: javascript:, data:, etc. are never linked.
void autolink_bare_urls(QString &s) {
  // URL chars: letters, digits, and the RFC 3986 unreserved + reserved
  // sub-delims we want to include. We do NOT include ')' or ',' because
  // those are common prose trailing punctuation. The leading context
  // group (^|[\s(>]) is non-capturing so the URL itself is capture(1).
  const QRegularExpression re(
      QStringLiteral(R"((?:^|[\s(>])(https?://[^\s<>\")']+))"),
      QRegularExpression::CaseInsensitiveOption);
  // Loop replace with a moving search offset so we always make
  // forward progress (the regex is non-overlapping, but degenerate
  // inputs could still loop if we re-replaced the same span).
  int search_from = 0;
  while (search_from < s.size()) {
    QRegularExpressionMatch m = re.match(s, search_from);
    if (!m.hasMatch())
      break;
    const QString url = m.captured(1);
    if (is_url_scheme_ok(url)) {
      const int url_start = m.capturedStart(1);
      const int url_len = url.size();
      const QString replacement =
          QStringLiteral("[url=%1]%2[/url]").arg(url, url);
      s.replace(url_start, url_len, replacement);
      // Skip past the inserted text so we don't recurse into the href
      // we just added.
      search_from = url_start + replacement.size();
    } else {
      // Skip past this match so the next iteration finds a different
      // URL (or terminates).
      search_from = m.capturedStart(1) + url.size();
    }
  }
}

// Linkify @mentions to a LoversLab profile-search page. The on-site
// mention anchor carries a numeric id and slug; for the plain-text
// JSON-LD / og:description path that id is not available, so we link
// to the search-by-name URL which 200s on a real username and falls
// back to LL's search results otherwise. The base URL is module-level
// so the linker shares it; tests can grep for "loverslab.com/profile/".
namespace {
constexpr const char kLoverslabProfileSearch[] =
    "https://www.loverslab.com/profile/?do=find&search=";
} // namespace

void linkify_mentions(QString &s) {
  // Match "@username" at start-of-string or after whitespace / opening
  // punctuation so we don't eat the '@' inside an email address or a
  // price like "1.50 @ $2". Username charset: A-Z a-z 0-9 _ - (matches
  // IPS / LL / Nexus username rules; '.' is excluded so trailing
  // punctuation like "@bob." does not pull the period into the
  // username). The leading context group is non-capturing so the
  // username is capture(1).
  const QRegularExpression re(
      QStringLiteral(R"((?:^|[\s(>])@([A-Za-z0-9][A-Za-z0-9_-]{0,63}))"));
  int search_from = 0;
  while (search_from < s.size()) {
    QRegularExpressionMatch m = re.match(s, search_from);
    if (!m.hasMatch())
      break;
    const QString name = m.captured(1);
    const QString href = QString::fromLatin1(kLoverslabProfileSearch) + name;
    const int name_start = m.capturedStart(1);
    const int name_len = name.size();
    const QString replacement =
        QStringLiteral("[url=%1]@%2[/url]").arg(href, name);
    s.replace(name_start, name_len, replacement);
    // Step past the inserted text so the regex doesn't re-match the
    // '@' we just wrapped. The replacement length is the authoritative
    // forward progress.
    search_from = name_start + replacement.size();
  }
}

} // namespace

// Thin Qt adapter over the vendored libcbb single-header BBCode-to-HTML
// library (third_party/libcbb/libcbb.h). The heavy lifting - tokenizing,
// tag stack, security sanitization (URL scheme allowlist, CSS meta-char
// rejection), and HTML escape - lives in libcbb.c. This TU only handles
// QString <-> UTF-8 std::string bridging, plus the input normalization
// the Nexus / Steam BBCode dialects need (entity unescape, CRLF, <br>,
// raw <a> anchors, bare URL autolink, @mention linkify, blank-line
// collapse).
//
// Normalization order matters:
//   1. CRLF -> LF: line endings must be a single canonical form before
//      any other pass.
//   2. unescape HTML entities: so an "&amp;lt;" from the API round-trips
//      to "<" via "&lt;" -> "<" (libcbb then re-escapes the '<' back to
//      "&lt;", rendering as the literal "<" character).
//   3. Convert raw <a href> anchors to BBCode [url] tags. The LoversLab
//      / Mod.pub scrapes feed HTML into the parser via the rich-text DOM
//      block (see loverslab/provider.cpp::parse_description_html), and
//      the Nexus API sometimes sprinkles <a> inside BBCode. libcbb has
//      no HTML parser so <a href=...> would render as escaped text.
//   4. <br> -> '\n' + dedup: the Nexus API mixes BBCode \n with literal
//      HTML <br>. Convert every <br> to '\n', then consume an adjacent
//      '\n' so "<br>\n" doesn't become a visible blank line under
//      pre-wrap.
//   5. Collapse 3+ blank lines down to a single paragraph break and
//      trim leading / trailing whitespace.
//   6. Autolink bare URLs so a copy-pasted https://... becomes a real
//      link. Run AFTER <br>-and-dedup so URLs that span a line break
//      are still captured. Run AFTER anchor conversion so we don't
//      re-link hrefs that are already inside [url] tags.
//   7. Linkify @mentions. Run last so we don't touch usernames inside
//      the URLs we just linked.
QString bbcode_to_html(const QString &input) {
  if (input.isEmpty()) {
    return {};
  }
  QString normalized = input;
  normalize_line_endings(normalized);
  normalized = unescape_html_entities(normalized);
  convert_html_anchors_to_bbcode(normalized);
  br_tags_to_newlines(normalized);
  collapse_blank_lines(normalized);
  autolink_bare_urls(normalized);
  linkify_mentions(normalized);
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
