// Tests for the BBCode-to-HTML adapter (ui/modinfo/bbcode.cpp) and the
// upstream normalization it applies to the Nexus / Steam BBCode dialects.
//
// The Nexus mods/{game}/mods/{id}.json `description` field arrives as a
// pre-escaped HTML/BBCode hybrid:
//   - BBCode tags ([b], [img], [url], ...) we want parsed.
//   - Pre-escaped HTML entities (&amp; &gt; &lt; &quot; &apos; &nbsp;)
//     that a stock BBCode parser will double-escape.
//   - Stray raw HTML (<br /> between paragraphs) that BBCode parsers
//     treat as text and escape to literal "&lt;br /&gt;".
//   - [img]https://...[/img] tags we want to keep as <img> elements.
//
// ui::bbcode_to_html() normalizes the input (entity unescape, br->newline)
// BEFORE handing it to libcbb, so the output is a clean HTML snippet that
// QTextBrowser can render correctly under white-space:pre-wrap.
//
// These tests exercise the public adapter; no Qt UI / QApplication needed.

#include "ui/modinfo/bbcode.h"

#include <QString>

#include <catch2/catch_test_macros.hpp>

namespace {

void check_html(const QString &input, const QString &want_substr) {
  const QString got = ui::bbcode_to_html(input);
  INFO("input=" + input.toStdString());
  INFO("got=" + got.toStdString());
  REQUIRE(got.contains(want_substr));
}

void check_html_equals(const QString &input, const QString &want) {
  const QString got = ui::bbcode_to_html(input);
  INFO("input=" + input.toStdString());
  INFO("got=" + got.toStdString());
  REQUIRE(got == want);
}

} // namespace

TEST_CASE("bbcode: raw <br> tags become a line break, not literal text",
          "[ui][bbcode]") {
  // All three common forms must be normalized to a raw \n that the
  // panel's white-space:pre-wrap wrapper will render as a break.
  // libcbb preserves \n verbatim (it only escapes <, >, &, ", ').
  check_html_equals(QStringLiteral("line 1<br />line 2"),
                    QStringLiteral("line 1\nline 2"));
  check_html_equals(QStringLiteral("a<br>b"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a<br/>b"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a<BR />b"), QStringLiteral("a\nb"));
  // Plain text without br must NOT introduce a spurious newline.
  check_html_equals(QStringLiteral("hello world"),
                    QStringLiteral("hello world"));
}

TEST_CASE("bbcode: pre-escaped entities round-trip through libcbb",
          "[ui][bbcode]") {
  // The Nexus API returns &amp; for a literal '&' in the source text.
  // Without normalization, libcbb escapes the '&' again -> &amp;amp;
  // and QTextBrowser shows literal "&amp;". After the unescape pass
  // the real '&' character goes back into libcbb, which re-escapes
  // it to "&amp;" - the correct, single-escaped display form.
  check_html_equals(QStringLiteral("A &amp; B"), QStringLiteral("A &amp; B"));
  check_html_equals(QStringLiteral("mainmenu &amp; loading menu replacers"),
                    QStringLiteral("mainmenu &amp; loading menu replacers"));
  // The "Gameplay -> Controller" arrow case from the bug report.
  check_html_equals(QStringLiteral("Gameplay &gt; Controller"),
                    QStringLiteral("Gameplay &gt; Controller"));
  check_html_equals(QStringLiteral("x &lt; y"), QStringLiteral("x &lt; y"));
  check_html_equals(QStringLiteral("&quot;hi&quot;"),
                    QStringLiteral("&quot;hi&quot;"));
  check_html_equals(QStringLiteral("it&apos;s"), QStringLiteral("it&#x27;s"));
  // &nbsp; becomes a plain space; libcbb does not escape space.
  check_html_equals(QStringLiteral("a&nbsp;b"), QStringLiteral("a b"));
  // Unknown entities pass through (libcbb re-escapes the '&').
  check_html_equals(QStringLiteral("&copy; 2026"),
                    QStringLiteral("&amp;copy; 2026"));
}

TEST_CASE("bbcode: numeric entities are decoded", "[ui][bbcode]") {
  // Decimal.
  check_html_equals(QStringLiteral("&#65;&#66;&#67;"), QStringLiteral("ABC"));
  // Hex (uppercase and lowercase).
  check_html_equals(QStringLiteral("&#x41;&#x42;&#x43;"),
                    QStringLiteral("ABC"));
  check_html_equals(QStringLiteral("&#X41;&#X42;&#X43;"),
                    QStringLiteral("ABC"));
  // A non-breaking space numeric reference - codepoint 160 decodes
  // to U+00A0 and round-trips through libcbb byte-for-byte (it only
  // escapes ASCII <, >, &, ", ').
  check_html_equals(QStringLiteral("a&#160;b"), QString::fromUtf8("a\xC2\xA0"
                                                                  "b"));
}

TEST_CASE("bbcode: single-pass unescape preserves double-escape intent",
          "[ui][bbcode]") {
  // If the API double-escapes (&amp;amp; -> literal "&amp;"), the
  // single-pass decoder should collapse ONE layer, giving "&amp;".
  // libcbb then re-escapes the '&' back to "&amp;amp;" - same as the
  // input. That's the right behaviour: we collapse at most one layer
  // of pre-escaping, which is what the Nexus / Steam feeds actually
  // do. A fully recursive unescape would corrupt user-intent strings.
  check_html_equals(QStringLiteral("A &amp;amp; B"),
                    QStringLiteral("A &amp;amp; B"));
}

TEST_CASE("bbcode: [img]https://...[/img] emits an <img> tag", "[ui][bbcode]") {
  // The image URL must survive normalization AND libcbb's URL sanitizer
  // (which allow-lists http/https/mailto/ftp/ftps). The output <img>
  // tag is what QTextBrowser will use to fetch the bytes through
  // DescriptionBrowser's loadResource override.
  check_html(
      QStringLiteral("[img]https://example.com/x.png[/img]"),
      QStringLiteral("<img src=\"https://example.com/x.png\" alt=\"\">"));
  check_html(
      QStringLiteral("[img=https://example.com/x.png]"),
      QStringLiteral("<img src=\"https://example.com/x.png\" alt=\"\">"));
  // The [IMG] form is case-folded by libcbb.
  check_html(
      QStringLiteral("[IMG]https://example.com/x.png[/IMG]"),
      QStringLiteral("<img src=\"https://example.com/x.png\" alt=\"\">"));
}

TEST_CASE("bbcode: dangerous [img] schemes are dropped", "[ui][bbcode]") {
  // libcbb's URL allow-list must reject javascript: and similar. The
  // tag drops entirely - we get empty output for the dropped section.
  // The remaining surrounding text still escapes correctly.
  const QString out = ui::bbcode_to_html(
      QStringLiteral("before [img]javascript:alert(1)[/img] after"));
  INFO("got=" + out.toStdString());
  REQUIRE(!out.contains(QStringLiteral("javascript")));
  REQUIRE(out.contains(QStringLiteral("before")));
  REQUIRE(out.contains(QStringLiteral("after")));
}

TEST_CASE("bbcode: combined Nexus-style description", "[ui][bbcode]") {
  // A realistic input that mixes every concern: BBCode bold, a raw
  // <br />, a pre-escaped &amp; / &gt;, and a remote [img] tag.
  const QString input =
      QStringLiteral("[b]Oathvein UI[/b]\nA mod for mainmenu &amp; loading "
                     "menu replacers.\n<br />"
                     "Gameplay &gt; Controller\n<br />"
                     "[img]https://staticdelivery.nexusmods.com/mods/1234/"
                     "images/160916-1.png[/img]");
  const QString got = ui::bbcode_to_html(input);
  INFO("got=" + got.toStdString());
  // Bold open + close still present.
  REQUIRE(got.contains(QStringLiteral("<b>Oathvein UI</b>")));
  // The & round-trips to &amp; for display, not &amp;amp;.
  REQUIRE(got.contains(QStringLiteral("mainmenu &amp; loading")));
  REQUIRE(!got.contains(QStringLiteral("&amp;amp;")));
  // The > round-trips to &gt; for display, not &amp;gt;.
  REQUIRE(got.contains(QStringLiteral("Gameplay &gt; Controller")));
  REQUIRE(!got.contains(QStringLiteral("&amp;gt;")));
  // The <br /> became \n, not literal text.
  REQUIRE(!got.contains(QStringLiteral("&lt;br")));
  REQUIRE(got.contains(QStringLiteral("\n")));
  // The [img] emitted an <img> tag.
  REQUIRE(got.contains(
      QStringLiteral("<img "
                     "src=\"https://staticdelivery.nexusmods.com/mods/1234/"
                     "images/160916-1.png\" alt=\"\">")));
}

TEST_CASE("bbcode: empty input returns empty", "[ui][bbcode]") {
  CHECK(ui::bbcode_to_html(QString()).isEmpty());
  CHECK(ui::bbcode_to_html(QStringLiteral("")).isEmpty());
  CHECK(ui::bbcode_to_html(QStringLiteral("   "))
            .contains(QStringLiteral("   ")));
}

TEST_CASE("bbcode: BBCode bold/italic/url still parse correctly",
          "[ui][bbcode]") {
  // Regression guard for the basics - our normalization must not
  // disturb BBCode tags that the API also emits.
  check_html_equals(QStringLiteral("[b]bold[/b]"),
                    QStringLiteral("<b>bold</b>"));
  check_html_equals(QStringLiteral("[i]italic[/i]"),
                    QStringLiteral("<i>italic</i>"));
  check_html_equals(QStringLiteral("[url=https://x.com]link[/url]"),
                    QStringLiteral("<a href=\"https://x.com\">link</a>"));
}
