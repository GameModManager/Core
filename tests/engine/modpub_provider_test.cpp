// Test for ModPubProvider::parse_mod_info - the pure HTML scrape behind
// the Mod Info ModPub tab "Refresh" button. mod.pub has no public API so
// this comes from the schema.org SoftwareApplication JSON-LD block on
// the public mod page. The parser is the mod.pub analog of
// loverslab_provider_test.cpp: feed a captured HTML snippet, assert the
// fields map cleanly, plus the mod.pub-specific game-slug handling.
#include "engine/source/modpub/provider.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
void require(bool cond, const char *msg) {
  INFO(msg);
  REQUIRE(cond);
}
} // namespace

TEST_CASE("modpub provider parse_mod_info", "[engine]") {
  using ModPubModInfoResult = engine::Source::ModPub::ModInfoResult;
  using engine::Source::ModPub::Provider;

  // --- Realistic captured mod page snippet. The JSON-LD block carries
  // schema.org SoftwareApplication with the same shape the live mod.pub
  // site emits: name, description, author.name, applicationCategory
  // GameMod (boilerplate), dateModified, and url. The Version field
  // lives in the DOM aside, not in the JSON.
  const std::string body =
      R"(<!DOCTYPE html>
<html>
<head>
  <title>Stay at the System Page NG - mod.pub</title>
  <meta property="og:title" content="Stay at the System Page NG" />
  <meta property="og:description" content="Stays at the system page on respawn." />
  <link rel="canonical" href="https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng" />
  <script type="application/ld+json">
  {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    "name": "Stay at the System Page NG",
    "url": "https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng",
    "author": { "@type": "Person", "name": "Eddoursul" },
    "description": "Stays at the system page on respawn.",
    "applicationCategory": "GameMod",
    "operatingSystem": "Windows",
    "datePublished": "2023-09-06 08:55:29",
    "dateModified": "2023-09-06 08:55:29",
    "offers": { "@type": "Offer", "price": "0" },
    "interactionStatistic": [
      {"@type": "InteractionCounter", "interactionType": "DownloadAction", "userInteractionCount": 51},
      {"@type": "InteractionCounter", "interactionType": "LikeAction", "userInteractionCount": 1}
    ]
  }
  </script>
</head>
<body>
  <aside>
    <b>Version</b> 1.0.0
    <b>Tag</b> User interface
  </aside>
</body>
</html>)";

  ModPubModInfoResult r =
      Provider::parse_mod_info(body, "https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng/");
  require(r.available, "available parsed");
  require(r.name == "Stay at the System Page NG", "name parsed");
  // applicationCategory is the boilerplate "GameMod" - the parser must
  // overwrite it with the DOM aside tag ("User interface") so the
  // panel shows a meaningful category.
  require(r.category == "User interface", "aside tag overrides boilerplate GameMod");
  require(r.author == "Eddoursul", "author object name parsed");
  require(r.description.find("Stays at the system page") != std::string::npos,
          "description parsed verbatim");
  require(r.date_modified == "2023-09-06 08:55:29",
          "dateModified parsed (out-of-date hook even if not displayed)");
  require(r.page_url.find("skyrim-se/22-stay-at-the-system-page-ng") != std::string::npos,
          "page_url parsed from JSON-LD url");
  // game_slug is back-filled from the fallback URL when JSON-LD did not
  // expose it directly. The Visit button uses it for the bare-URL
  // fallback.
  require(r.game_slug == "skyrim-se", "game_slug from fallback URL");

  // --- @graph-shaped JSON-LD: a future mod.pub change could wrap the
  // SoftwareApplication in a @graph with sibling types. The parser must
  // walk the graph and pick the SoftwareApplication entry.
  const std::string graph_body =
      R"(<script type="application/ld+json">
{
  "@context": "https://schema.org",
  "@graph": [
    {"@type": "WebSite", "name": "mod.pub"},
    {"@type": "SoftwareApplication", "name": "Graph Mod", "description": "Wrapped mod",
     "applicationCategory": "GameMod", "dateModified": "2025-05-12"}
  ]
}
</script>)";
  ModPubModInfoResult g =
      Provider::parse_mod_info(graph_body, "https://mod.pub/fallout4/77-graph-mod/");
  require(g.available, "graph: available");
  require(g.name == "Graph Mod", "graph: name extracted");
  require(g.date_modified == "2025-05-12", "graph: dateModified extracted");
  require(g.game_slug == "fallout4", "graph: game_slug from fallback URL");

  // --- No JSON-LD at all: fall back to og:* meta tags. og:description
  // is present so available=true.
  const std::string og_only =
      R"(<html><head>
<meta property="og:title" content="OG Title" />
<meta property="og:description" content="OG description text" />
<meta property="og:updated_time" content="2025-04-01" />
</head></html>)";
  ModPubModInfoResult o =
      Provider::parse_mod_info(og_only, "https://mod.pub/skyrim/12-og-title/");
  require(o.available, "og fallback: available");
  require(o.name == "OG Title", "og fallback: title");
  require(o.description == "OG description text", "og fallback: description");
  require(o.date_modified == "2025-04-01",
          "og fallback: og:updated_time used as dateModified");
  require(o.game_slug == "skyrim", "og fallback: game_slug from fallback URL");

  // --- Malformed JSON-LD: must NOT crash, must fall back to og:*.
  const std::string malformed =
      R"(<html><head>
<script type="application/ld+json">{not valid json</script>
<meta property="og:title" content="After Bad JSON" />
<meta property="og:description" content="Recovered description" />
</head></html>)";
  ModPubModInfoResult m =
      Provider::parse_mod_info(malformed, "https://mod.pub/enderalse/9-after-bad-json/");
  require(m.available, "malformed JSON-LD: og fallback works");
  require(m.name == "After Bad JSON",
          "malformed JSON-LD: og:title used as name");
  require(m.description == "Recovered description",
          "malformed JSON-LD: og:description used");
  require(m.game_slug == "enderalse", "malformed: game_slug from fallback URL");

  // --- No description: must report available=false even if name is set.
  // Distinguishes a parse failure from a page that just doesn't carry
  // the field, so the UI can show "Press Refresh" placeholder instead
  // of a half-populated row. The NSFW / Cloudflare login redirect also
  // maps to this case.
  const std::string no_desc =
      R"(<script type="application/ld+json">
{"@type":"SoftwareApplication","name":"Lonely Mod"}
</script>)";
  ModPubModInfoResult nd = Provider::parse_mod_info(no_desc, {});
  require(!nd.available, "missing description: available=false");

  // --- Empty body: available=false, no crash.
  ModPubModInfoResult empty = Provider::parse_mod_info({}, {});
  require(!empty.available, "empty body: available=false");

  // --- Garbage body: available=false, no crash.
  ModPubModInfoResult garbage =
      Provider::parse_mod_info("just some text, no html at all", {});
  require(!garbage.available, "garbage body: available=false");
}

TEST_CASE("modpub provider URL parsing", "[engine]") {
  using engine::Source::ModPub::Provider;

  // is_modpub_url: accepts scheme://mod.pub/... and bare mod.pub/...
  require(Provider::is_modpub_url("https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng"),
          "https scheme + path: recognized");
  require(Provider::is_modpub_url("http://mod.pub/fallout4/7-thing/"),
          "http scheme: recognized");
  require(Provider::is_modpub_url("mod.pub/enderalse/9-bare-paste/"),
          "bare mod.pub/ paste: recognized (no scheme)");
  require(!Provider::is_modpub_url("https://www.moddb.com/mods/1234"),
          "different host: not recognized");
  require(!Provider::is_modpub_url("https://nexusmods.com/skyrimspecialedition/mods/22"),
          "nexus: not recognized");
  require(!Provider::is_modpub_url(""), "empty: not recognized");

  // extract_mod_id: from a canonical URL with slug suffix
  require(Provider::extract_mod_id("https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng") == "22",
          "URL with slug: id 22");
  require(Provider::extract_mod_id("https://mod.pub/fallout4/7/") == "7",
          "URL with trailing slash, no slug: id 7");
  require(Provider::extract_mod_id("https://mod.pub/enderalse/99-some-mod-name") == "99",
          "URL with hyphenated slug: leading numeric is id");
  require(Provider::extract_mod_id("https://mod.pub/skyrim/12") == "12",
          "bare id: works");
  require(Provider::extract_mod_id("https://www.moddb.com/mods/1234").empty(),
          "wrong host: empty");
  require(Provider::extract_mod_id("https://mod.pub/files/foo") == "",
          "non-mod path: empty (no second segment)");

  // extract_game_slug: from the same URLs
  require(Provider::extract_game_slug("https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng") == "skyrim-se",
          "URL: game_slug skyrim-se");
  require(Provider::extract_game_slug("https://mod.pub/fallout4/7/") == "fallout4",
          "URL: game_slug fallout4");
  require(Provider::extract_game_slug("mod.pub/enderalse/9-bare-paste/") == "enderalse",
          "bare: game_slug enderalse");
  require(Provider::extract_game_slug("https://www.moddb.com/mods/1234").empty(),
          "wrong host: empty game_slug");

  // mod_page_url: strip query/fragment, ensure trailing slash
  require(Provider::mod_page_url("https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng?utm=1") ==
              "https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng/",
          "query stripped, trailing slash added");
  require(Provider::mod_page_url("https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng#files") ==
              "https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng/",
          "fragment stripped, trailing slash added");
  require(Provider::mod_page_url("https://mod.pub/fallout4/7/") ==
              "https://mod.pub/fallout4/7/",
          "trailing slash preserved");
  require(Provider::mod_page_url("https://mod.pub/enderalse/9-bare") ==
              "https://mod.pub/enderalse/9-bare/",
          "no trailing slash -> added");
}
