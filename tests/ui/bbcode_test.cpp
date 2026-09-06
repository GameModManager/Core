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

namespace
{

void check_html(const QString& input, const QString& want_substr)
{
  const QString got = ui::bbcode_to_html(input);
  INFO("input=" + input.toStdString());
  INFO("got=" + got.toStdString());
  REQUIRE(got.contains(want_substr));
}

void check_html_equals(const QString& input, const QString& want)
{
  const QString got = ui::bbcode_to_html(input);
  INFO("input=" + input.toStdString());
  INFO("got=" + got.toStdString());
  REQUIRE(got == want);
}

}  // namespace

TEST_CASE("bbcode: raw <br> tags become a line break, not literal text", "[ui][bbcode]")
{
  // All three common forms must be normalized to a raw \n that the
  // panel's white-space:pre-wrap wrapper will render as a break.
  // libcbb preserves \n verbatim (it only escapes <, >, &, ", ').
  check_html_equals(QStringLiteral("line 1<br />line 2"),
                    QStringLiteral("line 1\nline 2"));
  check_html_equals(QStringLiteral("a<br>b"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a<br/>b"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a<BR />b"), QStringLiteral("a\nb"));
  // Plain text without br must NOT introduce a spurious newline.
  check_html_equals(QStringLiteral("hello world"), QStringLiteral("hello world"));
}

TEST_CASE("bbcode: pre-escaped entities round-trip through libcbb", "[ui][bbcode]")
{
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
  check_html_equals(QStringLiteral("&quot;hi&quot;"), QStringLiteral("&quot;hi&quot;"));
  check_html_equals(QStringLiteral("it&apos;s"), QStringLiteral("it&#x27;s"));
  // &nbsp; becomes a plain space; libcbb does not escape space.
  check_html_equals(QStringLiteral("a&nbsp;b"), QStringLiteral("a b"));
  // Unknown entities pass through (libcbb re-escapes the '&').
  check_html_equals(QStringLiteral("&copy; 2026"), QStringLiteral("&amp;copy; 2026"));
}

TEST_CASE("bbcode: numeric entities are decoded", "[ui][bbcode]")
{
  // Decimal.
  check_html_equals(QStringLiteral("&#65;&#66;&#67;"), QStringLiteral("ABC"));
  // Hex (uppercase and lowercase).
  check_html_equals(QStringLiteral("&#x41;&#x42;&#x43;"), QStringLiteral("ABC"));
  check_html_equals(QStringLiteral("&#X41;&#X42;&#X43;"), QStringLiteral("ABC"));
  // A non-breaking space numeric reference - codepoint 160 decodes
  // to U+00A0 and round-trips through libcbb byte-for-byte (it only
  // escapes ASCII <, >, &, ", ').
  check_html_equals(QStringLiteral("a&#160;b"), QString::fromUtf8("a\xC2\xA0"
                                                                  "b"));
}

TEST_CASE("bbcode: single-pass unescape preserves double-escape intent", "[ui][bbcode]")
{
  // If the API double-escapes (&amp;amp; -> literal "&amp;"), the
  // single-pass decoder should collapse ONE layer, giving "&amp;".
  // libcbb then re-escapes the '&' back to "&amp;amp;" - same as the
  // input. That's the right behaviour: we collapse at most one layer
  // of pre-escaping, which is what the Nexus / Steam feeds actually
  // do. A fully recursive unescape would corrupt user-intent strings.
  check_html_equals(QStringLiteral("A &amp;amp; B"), QStringLiteral("A &amp;amp; B"));
}

TEST_CASE("bbcode: [img]https://...[/img] emits an <img> tag", "[ui][bbcode]")
{
  // The image URL must survive normalization AND libcbb's URL sanitizer
  // (which allow-lists http/https/mailto/ftp/ftps). The output <img>
  // tag is what QTextBrowser will use to fetch the bytes through
  // DescriptionBrowser's loadResource override.
  check_html(QStringLiteral("[img]https://example.com/x.png[/img]"),
             QStringLiteral("<img src=\"https://example.com/x.png\" alt=\"\">"));
  check_html(QStringLiteral("[img=https://example.com/x.png]"),
             QStringLiteral("<img src=\"https://example.com/x.png\" alt=\"\">"));
  // The [IMG] form is case-folded by libcbb.
  check_html(QStringLiteral("[IMG]https://example.com/x.png[/IMG]"),
             QStringLiteral("<img src=\"https://example.com/x.png\" alt=\"\">"));
}

TEST_CASE("bbcode: dangerous [img] schemes are dropped", "[ui][bbcode]")
{
  // libcbb's URL allow-list must reject javascript: and similar. The
  // tag drops entirely - we get empty output for the dropped section.
  // The remaining surrounding text still escapes correctly.
  const QString out =
      ui::bbcode_to_html(QStringLiteral("before [img]javascript:alert(1)[/img] after"));
  INFO("got=" + out.toStdString());
  REQUIRE(!out.contains(QStringLiteral("javascript")));
  REQUIRE(out.contains(QStringLiteral("before")));
  REQUIRE(out.contains(QStringLiteral("after")));
}

TEST_CASE("bbcode: combined Nexus-style description", "[ui][bbcode]")
{
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

TEST_CASE("bbcode: empty input returns empty", "[ui][bbcode]")
{
  CHECK(ui::bbcode_to_html(QString()).isEmpty());
  CHECK(ui::bbcode_to_html(QStringLiteral("")).isEmpty());
  // Whitespace-only input is trimmed by collapse_blank_lines so the
  // pre-wrap wrapper does not render a stray blank paragraph.
  CHECK(ui::bbcode_to_html(QStringLiteral("   ")).trimmed().isEmpty());
}

TEST_CASE("bbcode: BBCode bold/italic/url still parse correctly", "[ui][bbcode]")
{
  // Regression guard for the basics - our normalization must not
  // disturb BBCode tags that the API also emits.
  check_html_equals(QStringLiteral("[b]bold[/b]"), QStringLiteral("<b>bold</b>"));
  check_html_equals(QStringLiteral("[i]italic[/i]"), QStringLiteral("<i>italic</i>"));
  check_html_equals(QStringLiteral("[url=https://x.com]link[/url]"),
                    QStringLiteral("<a href=\"https://x.com\">link</a>"));
}

TEST_CASE("bbcode: CRLF line endings are normalized to LF", "[ui][bbcode]")
{
  // Nexus and Steam both emit "\r\n"; the wrapper uses pre-wrap, so a
  // stray '\r' would render as a visible control char. Normalize first.
  // The exact output is "\n" - libcbb passes raw newlines through.
  check_html_equals(QStringLiteral("a\r\nb"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a\rb"), QStringLiteral("a\nb"));
  // CRLF adjacent to a <br /> must not inflate into "\n\n\n".
  check_html_equals(QStringLiteral("a<br />\r\nb"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a\r\n<br />b"), QStringLiteral("a\nb"));
}

TEST_CASE("bbcode: <br> adjacent newlines are dedup'd", "[ui][bbcode]")
{
  // A <br> with a newline on either side is a single soft break, not
  // a paragraph break. The pre-pass must collapse to a single '\n'.
  check_html_equals(QStringLiteral("a<br>\nb"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a\n<br>b"), QStringLiteral("a\nb"));
  check_html_equals(QStringLiteral("a<br />\n<br />b"), QStringLiteral("a\nb"));
}

TEST_CASE("bbcode: 3+ blank lines collapse to a single paragraph break", "[ui][bbcode]")
{
  // Three or more '\n' in a row render as 2+ blank lines under pre-wrap.
  // The on-site visual is a single blank line between paragraphs.
  check_html_equals(QStringLiteral("a\n\n\nb"), QStringLiteral("a\n\nb"));
  check_html_equals(QStringLiteral("a\n\n\n\n\nb"), QStringLiteral("a\n\nb"));
  // Leading and trailing whitespace stripped.
  check_html_equals(QStringLiteral("\n\n\nfoo\n\n\n"), QStringLiteral("foo"));
  // A single blank line between paragraphs is preserved.
  check_html_equals(QStringLiteral("a\n\nb"), QStringLiteral("a\n\nb"));
}

TEST_CASE("bbcode: raw HTML <a href> anchors are converted to BBCode", "[ui][bbcode]")
{
  // The LoversLab / Mod.pub scrapes feed HTML into the parser via
  // the rich-text DOM block. libcbb is a BBCode parser, not an HTML
  // parser; without this conversion <a href=...> would render as
  // escaped text and the link would be lost.
  check_html(QStringLiteral("<a href=\"https://www.example.com\">click</a>"),
             QStringLiteral("<a href=\"https://www.example.com\">"
                            "click</a>"));
  // Single-quoted form.
  check_html(QStringLiteral("<a href='https://example.org'>visit</a>"),
             QStringLiteral("<a href=\"https://example.org\">"
                            "visit</a>"));
  // Anchor with extra attributes (class, rel, target) - common on
  // the LL/Mod.pub DOM. We pick the href regardless of attribute
  // order and ignore the rest.
  check_html(QStringLiteral("<a rel=\"external nofollow\" "
                            "href=\"https://x.com/\" class=\"link\">x</a>"),
             QStringLiteral("<a href=\"https://x.com/\">x</a>"));
  // Bad scheme: javascript: / data: / relative paths are dropped and
  // the surrounding text is kept. No <a> tag emitted, no surviving
  // "javascript" or "data:" substring.
  const QString js_out = ui::bbcode_to_html(
      QStringLiteral("prefix <a href=\"javascript:alert(1)\">bad</a> suffix"));
  INFO("got=" + js_out.toStdString());
  REQUIRE(!js_out.contains(QStringLiteral("javascript")));
  REQUIRE(!js_out.contains(QStringLiteral("<a ")));
  REQUIRE(js_out.contains(QStringLiteral("bad")));
  REQUIRE(js_out.contains(QStringLiteral("prefix")));
  REQUIRE(js_out.contains(QStringLiteral("suffix")));
}

TEST_CASE("bbcode: bare https URLs are autolinked", "[ui][bbcode]")
{
  // The user pastes a description that mentions a URL in prose. libcbb
  // has no autolinker, so a bare https://... stays as plain text. After
  // autolink_bare_urls it becomes a real <a>.
  check_html(QStringLiteral("see https://example.com/page for details"),
             QStringLiteral("<a href=\"https://example.com/page\">"
                            "https://example.com/page</a>"));
  // URL at the start of the string with no leading whitespace.
  check_html(QStringLiteral("https://example.com"),
             QStringLiteral("<a href=\"https://example.com\">"
                            "https://example.com</a>"));
  // A URL inside a BBCode tag is left alone - the user is already
  // controlling it. Our autolink regex requires (^|[\s(>]) before the
  // URL, and '[' is not in that set, so URLs inside [b]...[/b] stay
  // plain text. (Contrast: a stray URL in prose does get linked.)
  {
    const QString inside =
        ui::bbcode_to_html(QStringLiteral("[b]https://example.com[/b]"));
    INFO("got=" + inside.toStdString());
    // No <a> tag should appear inside the <b>...</b>.
    REQUIRE(!inside.contains(QStringLiteral("<a ")));
    REQUIRE(inside.contains(QStringLiteral("https://example.com")));
  }
  // javascript: bare URL is NOT autolinked (the prefix matched the
  // scheme regex but is_url_scheme_ok() rejects it; the text is kept
  // verbatim).
  const QString js_bare =
      ui::bbcode_to_html(QStringLiteral("see javascript:alert(1) here"));
  REQUIRE(!js_bare.contains(QStringLiteral("<a ")));
  REQUIRE(js_bare.contains(QStringLiteral("javascript:alert(1)")));
}

TEST_CASE("bbcode: @mentions are linkified to profile search", "[ui][bbcode]")
{
  // Plain-text JSON-LD descriptions carry "@username" without a link.
  // After linkify_mentions the @username becomes a [url=...]...[/url]
  // pointing at the LoversLab profile search, which resolves to the
  // real profile if the username exists.
  check_html(QStringLiteral("thanks @samuelga24 for the work"),
             QStringLiteral("samuelga24</a>"));
  // The URL must be the LL profile-search prefix - confirms we are
  // not making up an arbitrary external service. The '&' inside the
  // URL gets re-escaped to '&amp;' by libcbb on the way to the
  // browser; check for the escaped form.
  const QString got = ui::bbcode_to_html(QStringLiteral("hi @INueve"));
  REQUIRE(got.contains(
      QStringLiteral("https://www.loverslab.com/profile/?do=find&amp;search=INueve")));
  // @ at the very start (no leading whitespace) is also matched.
  const QString at_start =
      ui::bbcode_to_html(QStringLiteral("@samuelga24 created this"));
  REQUIRE(at_start.contains(QStringLiteral("samuelga24</a>")));
  // Email address must NOT be linkified as a mention.
  const QString email =
      ui::bbcode_to_html(QStringLiteral("contact me at user@example.com"));
  REQUIRE(!email.contains(QStringLiteral("user</a>")));
  // Punctuation boundary: trailing '.' or ',' does not get pulled into
  // the username.
  const QString trailing =
      ui::bbcode_to_html(QStringLiteral("thanks @alice, also @bob."));
  INFO("trailing=" + trailing.toStdString());
  REQUIRE(trailing.contains(QStringLiteral("@alice</a>")));
  REQUIRE(trailing.contains(QStringLiteral("@bob</a>")));
}

TEST_CASE("bbcode: combined realistic Skooma-style description", "[ui][bbcode]")
{
  // Mirrors the on-site Skooma Whore SE description shape (LL rich-text
  // DOM): a <p> with a bare URL, a mention, an <a> anchor, and a <br>.
  // The combined-pipeline assertions cover ordering bugs that would
  // silently break the individual passes.
  //
  // Note: libcbb is a BBCode parser, NOT an HTML parser. Unrecognized
  // tags like <p> become escaped text ("&lt;p&gt;") - they are NOT
  // stripped or converted to paragraph breaks. The DOM->BBCode
  // conversion for the LL / ModPub providers (which feeds real HTML
  // through this path) is a separate concern; for THIS test the
  // BBCode-in / BBCode-out path is what we cover, so the assertion
  // is that the <p> tags round-trip as escaped text and the links /
  // mentions survive.
  const QString input = QStringLiteral(
      "<p>Thanks @samuelga24 for the DAR anim.</p>\r\n"
      "<p>Get it here: <a href=\"https://www.loverslab.com/files/file/1234\">"
      "Skooma Whore SE</a> or https://www.nexusmods.com/skyrimspecialedition/"
      "mods/789</p>\r\n<br />\r\nMore info");
  const QString got = ui::bbcode_to_html(input);
  INFO("got=" + got.toStdString());
  // The mention links to the LL profile search (& escaped by libcbb).
  REQUIRE(got.contains(QStringLiteral(
      "https://www.loverslab.com/profile/?do=find&amp;search=samuelga24")));
  // The LoversLab anchor is preserved as a real link.
  REQUIRE(got.contains(
      QStringLiteral("<a href=\"https://www.loverslab.com/files/file/1234\">"
                     "Skooma Whore SE</a>")));
  // The bare Nexus URL is autolinked.
  REQUIRE(got.contains(QStringLiteral(
      "<a href=\"https://www.nexusmods.com/skyrimspecialedition/mods/789\">"
      "https://www.nexusmods.com/skyrimspecialedition/mods/789</a>")));
  // The <p> tag is unknown to libcbb and is escaped to literal text.
  // The tag itself is preserved; the BBCode parser does not infer
  // block structure from it. The newline-collapse pass still caps
  // blank lines to one.
  REQUIRE(!got.contains(QStringLiteral("\n\n\n")));
  // The <br /> was consumed (now a soft break, not a literal tag).
  REQUIRE(!got.contains(QStringLiteral("<br")));
}
