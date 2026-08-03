#pragma once

#include "engine/source/source_provider.h"

#include <string>

namespace engine {

struct NxmLink;

// Result of a mods/{game}/mods/{id}.json query. `available` is false when the
// request failed (no API key, HTTP error, unparseable body).
struct ModInfoResult {
    bool available = false;
    std::string name;
    std::string version;          // current installed-file version
    std::string newest_version;   // newest file version on Nexus
    std::string category_id;      // Nexus category id
    std::string description;      // Nexus BBCode description
    std::string author;
};

class NexusProvider : public SourceProvider {
public:
    std::string source_type() const override { return "nexus"; }
    bool fetch(const Mod& mod, PipelineContext& ctx,
               const std::filesystem::path& dest_path) override;
    // Returns the real Nexus file metadata (files/{file}.json): the archive
    // name (file_name) for correct naming/extension and the display name.
    SourceDownloadInfo resolve_download_info(const Mod& mod) const override;
    std::string display_name() const override;

    // Live mod-info lookup for the Mod Info Nexus tab ("Refresh" button).
    // Requires a configured API key; fills ModInfoResult::available=false on
    // any failure. Never throws.
    ModInfoResult fetch_mod_info(const std::string& nexus_domain,
                                 const std::string& mod_id) const;

    // Pure body parser for the mods/{game}/mods/{id}.json response (extracted
    // so the mapping is unit-testable without the network).
    static ModInfoResult parse_mod_info(const std::string& body);

private:
    // Shared download routine for a resolved URL (used by both the API-auth
    // paths and the direct-URL path). server_name is the Nexus mirror that
    // served the URL (from the download_link.json entry); when non-empty a
    // speed sample is recorded against it on success (MO2 parity).
    bool download_from_url(const std::string& url, PipelineContext& ctx,
                           const std::filesystem::path& dest_path,
                           const std::string& server_name = {});
};

} // namespace engine
