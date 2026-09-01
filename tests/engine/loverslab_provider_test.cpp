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
} // namespace

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
  require(m.name == "After Bad JSON",
          "malformed JSON-LD: og:title used as name");
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
}