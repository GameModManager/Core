#pragma once

#include "engine/source/interface.h"

#include <string>

namespace engine::Source::LoversLab {

// Result of a loverslab.com/files/file/{id}/ scrape (the Mod Info LoversLab
// tab's Refresh button). LoversLab has no API so this comes from the
// schema.org WebApplication JSON-LD embedded in the public mod page; the
// dateModified field is the "Updated June 5" stamp on the page (used for
// out-of-date detection against the user's install timestamp). `available`
// is false when the request failed (network error, parse failure, missing
// required fields).
struct ModInfoResult {
    bool available = false;
    std::string name;
    std::string version;           // softwareVersion from JSON-LD
    std::string category;          // applicationCategory
    std::string description;       // plain-text description from JSON-LD
    std::string author;            // author.name
    std::string page_url;          // canonical /files/file/{id}/ URL
    // ISO 8601 timestamp of the last mod update (e.g. "2025-06-05" or
    // "2025-06-05T12:34:56"). Empty when the page did not advertise one.
    std::string date_modified;
};

// Downloads LoversLab files. LoversLab has no public API: downloads are
// authorized by the user's browser session, so this provider sends the stored
// session cookie (Auth) as the Cookie header while fetching the
// pasted ?do=download&r=<revision>&confirm=1&t=1&csrfKey=<key> link. The
// archive name is resolved from the final response's Content-Disposition (or
// the effective URL path) so .7z/.rar/.zip archives keep their real extension.
class Provider : public Interface {
public:
    std::string source_type() const override { return "loverslab"; }
    bool fetch(const ::engine::Mod& mod, ::engine::PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    SourceDownloadInfo resolve_download_info(const ::engine::Mod& mod) const override;
    std::string display_name() const override;

    // URL helpers (pure, unit-tested, reused by the future browser-extension
    // flow that will hand GMM a bare file-page link).
    static bool is_loverslab_url(const std::string& url);
    // "https://www.loverslab.com/files/file/12345-slug/?do=download..." -> "12345"
    static std::string extract_file_id(const std::string& url);
    // "https://www.loverslab.com/files/file/12345-slug/?do=download&..." ->
    // "https://www.loverslab.com/files/file/12345-slug/" (query/fragment
    // stripped). A bare page link is returned unchanged.
    static std::string mod_page_url(const std::string& url);

    // Scrape the public mod page (no cookie, guest fetch; metadata is
    // guest-visible, downloads are not). Accepts either a full
    // loverslab.com/files/file/... URL or a bare file id (string of digits).
    // Returns ModInfoResult::available=false on any failure (network error,
    // HTTP != 200, parse failure). Never throws.
    ModInfoResult fetch_mod_info(const std::string& file_id_or_url) const;

    // Pure body parser for the fetched page - extracted so the mapping is
    // unit-testable without the network. Pulls the schema.org WebApplication
    // JSON-LD block (with og:* meta fallback) and fills the result. Sets
    // available=true only when a name AND description are present.
    static ModInfoResult parse_mod_info(const std::string& html_body);
};

} // namespace engine::Source::LoversLab
