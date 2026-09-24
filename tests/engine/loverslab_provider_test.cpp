// Test for LoversLabProvider::parse_mod_info - the pure HTML scrape behind the
// Mod Info LoversLab tab "Refresh" button. The parser pulls the schema.org
// WebApplication JSON-LD block from the guest-visible mod page (the page is
// guest-visible; downloads are not). Mirrors nexus_provider_test.cpp's
// approach: feed a captured HTML snippet, assert the fields map cleanly.
#include "engine/source/loverslab_provider.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <string>

namespace {
void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
}  // namespace

TEST_CASE("loverslab provider", "[engine]") {
  using engine::LoversLabModInfoResult;
  using engine::LoversLabProvider;

  // --- Realistic captured mod page snippet. The script tags and meta tags
  // here mirror what the live Invision Community page emits: a single
  // WebApplication block in JSON-LD, plus a few og:* mirror tags.
  const std::string body =
      R"(<!DOCTYPE html>
<html>
<head>
  <title>The Xims Magazine - LoversLab</title>
  <meta property="og:title" content="The Xims Magazine" />
  <meta property="og:description" content="Hi all, attached is the latest issue." />
  <meta property="og:image" content="https://www.loverslab.com/uploads/monthly_2025_06/xims.png" />
  <script type="application/ld+json">
  {
    "@context": "https://schema.org",
    "@type": "WebApplication",
    "name": "The Xims Magazine",
    "url": "https://www.loverslab.com/files/file/11488-the-xims-magazine/",
    "author": { "@type": "Person", "name": "INueve" },
    "description": "Hi all, attached is the latest issue. Lots of content inside.",
    "softwareVersion": "1.0",
    "applicationCategory": "Objects",
    "dateModified": "2025-06-05T14:23:11",
    "fileSize": "3.06 MB",
    "interactionStatistic": [
      {"@type": "InteractionCounter", "interactionType": "ViewAction", "userInteractionCount": 50138},
      {"@type": "InteractionCounter", "interactionType": "DownloadAction", "userInteractionCount": 10498}
    ]
  }
  </script>
  <script type="application/ld+json">
  {
    "@context": "https://schema.org",
    "@type": "BreadcrumbList",
    "itemListElement": []
  }
  </script>
</head>
<body>...</body>
</html>)";

  LoversLabModInfoResult r = LoversLabProvider::parse_mod_info(body);
  require(r.available, "available parsed");
  require(r.name == "The Xims Magazine", "name parsed");
  require(r.version == "1.0", "softwareVersion -> version");
  require(r.category == "Objects", "applicationCategory -> category");
  require(r.author == "INueve", "author object name parsed");
  require(r.description.find("Hi all") != std::string::npos,
          "description parsed verbatim");
  require(r.date_modified == "2025-06-05T14:23:11",
          "dateModified parsed (used for out-of-date detection)");
  require(r.page_url.find("/files/file/11488") != std::string::npos,
          "page_url parsed from JSON-LD url");

  // --- @graph-shaped JSON-LD: Invision Community sometimes wraps the
  // WebApplication in @graph with sibling types. The parser must walk the
  // graph and pick the WebApplication entry.
  const std::string graph_body =
      R"(<script type="application/ld+json">
{
  "@context": "https://schema.org",
  "@graph": [
    {"@type": "WebSite", "name": "LoversLab"},
    {"@type": "WebApplication", "name": "Graph Mod", "description": "Wrapped mod",
     "softwareVersion": "2.5", "dateModified": "2025-05-12"}
  ]
}
</script>)";
  LoversLabModInfoResult g = LoversLabProvider::parse_mod_info(graph_body);
  require(g.available, "graph: available");
  require(g.name == "Graph Mod", "graph: name extracted");
  require(g.version == "2.5", "graph: softwareVersion extracted");
  require(g.date_modified == "2025-05-12",
          "graph: dateModified extracted (out-of-date check still works)");

  // --- No JSON-LD at all: fall back to og:* meta tags. og:description is
  // present so available=true; dateModified falls back to og:updated_time.
  const std::string og_only =
      R"(<html><head>
<meta property="og:title" content="OG Title" />
<meta property="og:description" content="OG description text" />
<meta property="og:updated_time" content="2025-04-01" />
</head></html>)";
  LoversLabModInfoResult o = LoversLabProvider::parse_mod_info(og_only);
  require(o.available, "og fallback: available");
  require(o.name == "OG Title", "og fallback: title");
  require(o.description == "OG description text", "og fallback: description");
  require(o.date_modified == "2025-04-01",
          "og fallback: og:updated_time used as dateModified");

  // --- Date-only JSON-LD dateModified - the common Invision Community
  // shape ("Updated June 5" displays as just "2025-06-05").
  const std::string date_only =
      R"(<script type="application/ld+json">
{"@type":"WebApplication","name":"Date Only","description":"d","dateModified":"2025-06-05"}
</script>)";
  LoversLabModInfoResult d = LoversLabProvider::parse_mod_info(date_only);
  require(d.available, "date-only: available");
  require(d.date_modified == "2025-06-05", "date-only dateModified kept");

  // --- Malformed JSON-LD: must NOT crash, must fall back to og:*.
  const std::string malformed =
      R"(<html><head>
<script type="application/ld+json">{not valid json</script>
<meta property="og:title" content="After Bad JSON" />
<meta property="og:description" content="Recovered description" />
</head></html>)";
  LoversLabModInfoResult m = LoversLabProvider::parse_mod_info(malformed);
  require(m.available, "malformed JSON-LD: og fallback works");
  require(m.name == "After Bad JSON", "malformed JSON-LD: og:title used as name");
  require(m.description == "Recovered description",
          "malformed JSON-LD: og:description used");

  // --- No description: must report available=false even if name is set.
  // Distinguishes a parse failure from a page that just doesn't carry
  // the field, so the UI can show "Press Refresh" placeholder instead of
  // a half-populated row.
  const std::string no_desc =
      R"(<script type="application/ld+json">
{"@type":"WebApplication","name":"Lonely Mod"}
</script>)";
  LoversLabModInfoResult nd = LoversLabProvider::parse_mod_info(no_desc);
  require(!nd.available, "missing description: available=false");

  // --- Empty body: available=false, no crash.
  LoversLabModInfoResult empty = LoversLabProvider::parse_mod_info({});
  require(!empty.available, "empty body: available=false");

  // --- Garbage body: available=false, no crash.
  LoversLabModInfoResult garbage =
      LoversLabProvider::parse_mod_info("just some text, no html at all");
  require(!garbage.available, "garbage body: available=false");

  // --- Attribute-order-agnostic <meta>: some themes emit the content
  // attribute BEFORE the property/name attribute. The og:* fallback path
  // must pick the content value regardless of attribute order.
  const std::string content_first =
      R"(<html><head>
<meta content="Reversed Title" property="og:title">
<meta content="Reversed description text" property="og:description">
</head></html>)";
  LoversLabModInfoResult cf = LoversLabProvider::parse_mod_info(content_first);
  require(cf.available, "content-first meta: available (regex handles reversed order)");
  require(cf.name == "Reversed Title", "content-first meta: og:title picked");
  require(cf.description == "Reversed description text",
          "content-first meta: og:description picked");

  // --- Same with name= instead of property= (some themes use name for
  // og:* tags). property= is conventional but accept both.
  const std::string name_first =
      R"(<html><head>
<meta name="og:title" content="Named Title">
<meta name="og:description" content="Named description text">
</head></html>)";
  LoversLabModInfoResult nf = LoversLabProvider::parse_mod_info(name_first);
  require(nf.available, "name= meta: available");
  require(nf.name == "Named Title", "name= meta: og:title picked");
  require(nf.description == "Named description text",
          "name= meta: og:description picked");

  // --- When BOTH JSON-LD and the rich-text block carry a description,
  // the rich-text version WINS - schema.org `description` is plain
  // text by contract, so any link/mention the page actually shows
  // would be lost if we trusted JSON-LD. The earlier `if
  // (result.description.empty())` gate made rich a fallback that
  // almost never ran (JSON-LD is present on every LL page); the
  // spec-correct precedence is "rich always when available".
  const std::string both =
      R"(<html><head>
<script type="application/ld+json">
{"@type":"WebApplication","name":"Both Mod","description":"plain text JSON-LD"}
</script>
</head><body>
<div class="ipsType_richText"><p>rich <a href="https://www.example.com">link</a> text</p></div>
</body></html>)";
  LoversLabModInfoResult b = LoversLabProvider::parse_mod_info(both);
  require(b.available, "both: available");
  require(b.name == "Both Mod", "both: JSON-LD name used for name");
  // Rich text wins for description: the string "plain text JSON-LD"
  // must NOT appear; the link "https://www.example.com" MUST.
  require(b.description.find("plain text JSON-LD") == std::string::npos,
          "both: rich text beats JSON-LD plain-text description");
  require(b.description.find("https://www.example.com") != std::string::npos,
          "both: rich text preserves the link the user sees on-site");
}

TEST_CASE("loverslab provider description_html block", "[engine][loverslab]") {
  using engine::LoversLabProvider;
  // --- Realistic Invision Community "About This File" block: the
  // class="ipsType_richText" container holds <p>...</p> paragraphs, a
  // real <a href> anchor, and a plain text run. The parser must:
  //   - locate the right <div> (not the changelog's, not unrelated
  //     widgets on the page),
  //   - return the raw inner HTML verbatim (block + inline tags intact)
  //     for the renderer's set_description path - no BBCode round-trip,
  //     no tag stripping - and
  //   - leave the visible text intact so the user sees a real
  //     description (not a "go to the site" stub).
  const std::string body =
      R"(<html><body>
<section class="ipsType_normal">
  <h2>About This File</h2>
  <div class="ipsType_richText ipsContained ipsType_break ipsSpacer_bottom">
    <p>Hi all, attached is the latest issue.</p>
    <p>The original CC can be found at <a href="https://www.example.com/foo">https://www.example.com/foo</a> (credit to the original creator).</p>
    <p>Thanks <span style="color:#f1c40f;"><strong>wtrshpdwn</strong></span> for the assist.</p>
  </div>
</section>
<section>
  <h2>What's New</h2>
  <div class="ipsType_richText">
    <p>No changelog available for this version.</p>
  </div>
</section>
</body></html>)";
  const std::string got = LoversLabProvider::parse_description_html(body);
  require(!got.empty(), "rich-text block extracted");
  // Safe links survive as real <a> tags - no BBCode round-trip, so the
  // renderer receives the markup the site authored.
  require(got.find("<a href=\"https://www.example.com/foo\">") != std::string::npos,
          "safe anchor preserved verbatim");
  // Block markup survives: paragraphs stay <p> for the renderer (no
  // '\n\n' join - the tags carry the breaks).
  require(got.find("<p>Hi all, attached is the latest issue.</p>") != std::string::npos,
          "paragraph markup preserved");
  // Inline formatting tags (<span>, <strong>) survive with their text.
  require(got.find("<strong>wtrshpdwn</strong>") != std::string::npos,
          "inline formatting tags preserved");
  require(got.find("wtrshpdwn") != std::string::npos,
          "inner text of <strong> preserved");
  // The changelog block (also ipsType_richText but further down) is
  // NOT picked - we only want the first one and that's the "About
  // This File" container, not the changelog.
  require(got.find("changelog") == std::string::npos,
          "changelog block is not extracted");

  // --- Anchors with disallowed schemes: javascript:, data:, relative
  // paths. The anchor tag collapses to its visible text (no executable
  // href reaches the renderer).
  const std::string bad =
      R"DELIM(<div class="ipsType_richText">before <a href="javascript:alert(1)">bad</a> after</div>)DELIM";
  const std::string bad_got = LoversLabProvider::parse_description_html(bad);
  require(bad_got.find("javascript") == std::string::npos,
          "javascript: scheme is dropped");
  require(bad_got.find("bad") != std::string::npos,
          "link text preserved when scheme is bad");
  require(bad_got == "before bad after", "unsafe anchor collapses exactly to its text");

  // --- Single-quoted safe href: preserved verbatim (sanitizer keeps the
  // whole tag, quotes and all).
  const std::string sq_anchor =
      R"DELIM(<div class="ipsType_richText"><a href='https://www.example.com/x'>quoted</a></div>)DELIM";
  require(LoversLabProvider::parse_description_html(sq_anchor) ==
              "<a href='https://www.example.com/x'>quoted</a>",
          "single-quoted safe anchor preserved verbatim");

  // --- No rich-text block at all: empty result, no crash.
  require(LoversLabProvider::parse_description_html("just text").empty(),
          "no block: empty");
  require(LoversLabProvider::parse_description_html({}).empty(), "empty body: empty");

  // --- Single-quoted class attribute. Some LL themes / mod.pub render
  // the class attribute with single quotes; the regex used to require
  // a double quote and silently dropped the block. The fix matches
  // either quote.
  const std::string single_quote =
      R"DELIM(<div class='ipsType_richText'><p>quoted class survives</p></div>)DELIM";
  const std::string sq_got = LoversLabProvider::parse_description_html(single_quote);
  require(!sq_got.empty(), "single-quoted class: block extracted");
  require(sq_got.find("quoted class survives") != std::string::npos,
          "single-quoted class: text preserved");
}

TEST_CASE("loverslab description raw html passthrough", "[engine][loverslab]") {
  using engine::LoversLabProvider;
  // --- Raw-HTML mode: the inner fragment reaches the renderer verbatim.
  // <br> stays a tag (the WebEngine view renders it natively), so the
  // old <br>-to-newline dedup no longer applies - what the site
  // authored is what the user sees.
  const std::string br_nl =
      R"DELIM(<div class="ipsType_richText">line1<br>
line2</div>)DELIM";
  require(LoversLabProvider::parse_description_html(br_nl) == "line1<br>\nline2",
          "<br> + newline preserved verbatim");

  // --- Same with CRLF bodies (real HTTP pages use \r\n): verbatim too,
  // the renderer normalizes line endings.
  const std::string br_crlf =
      "<div class=\"ipsType_richText\">line1<br />\r\nline2</div>";
  require(LoversLabProvider::parse_description_html(br_crlf) == "line1<br />\r\nline2",
          "<br /> + CRLF preserved verbatim");

  // --- A doubled <br><br> (author-intended gap) stays two tags.
  const std::string br_br =
      R"DELIM(<div class="ipsType_richText">a<br><br>b</div>)DELIM";
  require(LoversLabProvider::parse_description_html(br_br) == "a<br><br>b",
          "<br><br> preserved verbatim");

  // --- Pretty-printed paragraphs: block tags and their indentation
  // survive untouched - the renderer lays them out.
  const std::string pretty = "<div class=\"ipsType_richText\">\n"
                             "    <p>para1</p>\n"
                             "    <p>para2</p>\n"
                             "  </div>";
  require(LoversLabProvider::parse_description_html(pretty) ==
              "\n    <p>para1</p>\n    <p>para2</p>\n  ",
          "pretty-printed block markup preserved verbatim");
}

TEST_CASE("loverslab metadata entity decoding", "[engine][loverslab]") {
  using engine::LoversLabModInfoResult;
  using engine::LoversLabProvider;
  // --- Workspace-vbx3: JSON-LD text fields arrive HTML-escaped from
  // the page; the panel shows them verbatim so they must be decoded
  // at parse time (the og:* fallback path always did).
  const std::string body =
      R"(<script type="application/ld+json">
{"@type":"WebApplication","name":"Swords &amp; Sorcery",
 "description":"plain description",
 "applicationCategory":"Framework &amp; Resources",
 "author":{"name":"Modder &quot;Bob&quot;"}}
</script>)";
  LoversLabModInfoResult r = LoversLabProvider::parse_mod_info(body);
  require(r.available, "entities: available");
  require(r.name == "Swords & Sorcery", "entities: name decoded");
  require(r.category == "Framework & Resources", "entities: category decoded");
  require(r.author == "Modder \"Bob\"", "entities: author decoded");

  // --- og:* fallback with entities still decodes (read_meta refactor
  // must not regress the path that always worked).
  const std::string og =
      R"(<html><head>
<meta property="og:title" content="Fish &amp; Chips" />
<meta property="og:description" content="Tasty &lt;food&gt;" />
</head></html>)";
  LoversLabModInfoResult o = LoversLabProvider::parse_mod_info(og);
  require(o.available, "og entities: available");
  require(o.name == "Fish & Chips", "og entities: title decoded");
  require(o.description == "Tasty <food>", "og entities: description decoded");
}