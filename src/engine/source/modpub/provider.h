#pragma once

#include "engine/source/interface.h"

#include <string>

namespace engine::Source::ModPub {

// Result of a mod.pub page scrape (the Mod Info ModPub tab's Refresh
// button). mod.pub has no public API so this comes from the schema.org
// SoftwareApplication JSON-LD block embedded in the public mod page; the
// dateModified field is the page's "Updated" stamp (used for out-of-date
// detection against the user's install timestamp). `available` is false
// when the request failed (network error, parse failure, missing required
// fields, NSFW/Cloudflare gate that bounced the guest GET to a login
// page).
//
// Difference from LoversLab: mod.pub's page URL is not reconstructible
// from the mod id alone - the URL is always
//   https://mod.pub/<game-slug>/<id>-<slug-suffix>
// and the game-slug is part of the identity (mods live under a per-game
// path). The panel keeps the page URL in [ModPub] and `page_url` carries
// the canonical https://mod.pub/<game>/<id>-<suffix> form so a reinstall
// can re-derive the modl:// link (or the Visit button) without the slug.
struct ModInfoResult {
    bool available = false;
    std::string name;
    std::string version;           // softwareVersion from JSON-LD (may be
                                   // empty - mod.pub shows Version in the
                                   // DOM aside, not in the JSON)
    std::string category;          // applicationCategory (e.g. "GameMod")
                                   // - mod.pub tags the mod separately
                                   // (e.g. "User interface") and the
                                   // panel falls back to the DOM tag
                                   // when this is empty
    std::string description;       // plain-text description from JSON-LD
    std::string author;            // author.name (e.g. "Eddoursul")
    std::string page_url;          // canonical https://mod.pub/<slug>/<id>-<suffix>
    // mod.pub game path (e.g. "skyrim-se"). Empty when the page did not
    // advertise one. Used by `Visit on ModPub` to build the bare
    // https://mod.pub/<slug>/<id>/ fallback when [ModPub] page_url is
    // missing the slug suffix.
    std::string game_slug;
    // ISO 8601 timestamp of the last mod update (e.g. "2023-09-06" or
    // "2023-09-06T08:55:29"). Empty when the page did not advertise one.
    std::string date_modified;
};

// ModPub is a metadata-only source: mod.pub itself does not expose a
// public download API. Downloads reach the app via the companion modl://
// protocol (which the mod.pub site emits as a "MO2 download" button and
// a downloadable URL the user's browser hands to the OS handler). The
// ModPub provider therefore exists for:
//   * registry registration (so SourceTab/AddSourceDialog can attribute
//     a mod to "modpub" - the single-source rule fqf5 forbids folding
//     modpub into nexus)
//   * the panel's Refresh button (guest GET + JSON-LD scrape)
//   * source_page_url / Visit routing
// `fetch()` and `resolve_download_info()` are stubs: they refuse to run so
// FetchStage aborts cleanly if anything ever does route a "modpub" mod
// through the pipeline (the actual install path is "modl" - DownloadsTab
// hands the file to the modl provider).
class Provider : public Interface {
public:
    std::string source_type() const override { return "modpub"; }
    bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    SourceDownloadInfo resolve_download_info(const ::engine::Mod& mod) const override;
    std::string display_name() const override;

    // URL helpers (pure, unit-tested). mod.pub canonical URL shape:
    //   https://mod.pub/<game-slug>/<id>-<slug-suffix>
    // Optional trailing slash; the host is always "mod.pub" (no www).
    // The id is the first numeric segment after the game-slug; everything
    // after the dash is the human-readable slug and is preserved (we
    // cannot reconstruct it from the id alone, which is why we keep the
    // full page URL in [ModPub] page_url).
    static bool is_modpub_url(const std::string& url);
    // Extract the numeric mod id from a canonical mod.pub URL. Returns
    // empty when the URL does not look like mod.pub. Accepts:
    //   https://mod.pub/<game>/<id>-slug   -> "<id>"
    //   https://mod.pub/<game>/<id>        -> "<id>"
    //   https://mod.pub/<game>/<id>/       -> "<id>"
    // The mod id is the leading numeric run before the first '-' or '/'.
    static std::string extract_mod_id(const std::string& url);
    // Extract the game-slug from a canonical mod.pub URL. Returns empty
    // when the URL has no slug (e.g. a bare mod id).
    static std::string extract_game_slug(const std::string& url);
    // Strip query/fragment from a mod.pub URL; return the canonical page
    // form (always with a trailing slash so the URL the user pastes
    // matches what mod.pub itself renders).
    static std::string mod_page_url(const std::string& url);

    // Scrape the public mod page (no cookie, guest fetch; metadata is
    // guest-visible, downloads are not). Accepts either a full mod.pub
    // URL or a "game_slug/mod_id" pair. Returns ModInfoResult::available=
    // false on any failure (network error, HTTP != 200, NSFW/CF
    // redirect, parse failure). Never throws.
    ModInfoResult fetch_mod_info(const std::string& url_or_id) const;

    // Pure body parser for the fetched page - extracted so the mapping is
    // unit-testable without the network. Pulls the schema.org
    // SoftwareApplication JSON-LD block (with og:* meta fallback) and
    // fills the result. Sets available=true only when a name AND
    // description are present.
    static ModInfoResult parse_mod_info(const std::string& html_body,
                                        const std::string& fallback_url);

    // Pure body parser that extracts a rich-text description block from
    // the page (mod.pub's content area). Returns empty when the page
    // has no recognizable block - the parser prefers the JSON-LD
    // description in that case. The shared bbcode_to_html() pipeline
    // improvements (anchor pass, autolink, mentions) still benefit any
    // HTML that does arrive, so callers that already have HTML do not
    // need to special-case it.
    static std::string parse_description_html(const std::string& html_body) {
      (void)html_body;
      return {};
    }
};

} // namespace engine::Source::ModPub

// Backward-compat alias kept at file scope so consumers (the modpub fetch
// worker, the ModPub mod-info panel, the ModInfoData lambda return type)
// can spell it as engine::ModPubModInfoResult without a separate include
// of the UI mod_info_data.h header. Mirrors the LoversLab pattern in
// engine/source/loverslab_provider.h.
namespace engine {
using ModPubModInfoResult = Source::ModPub::ModInfoResult;
} // namespace engine
