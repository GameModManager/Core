#pragma once

#include "engine/source/interface.h"
#include "engine/source/router.h"

#include <string>
#include <string_view>
#include <vector>

namespace engine::Source::Nexus {

// Result of a mods/{game}/mods/{id}.json query. `available` is false when the
// request failed (no API key, HTTP error, unparseable body).
struct ModInfoResult {
  bool available = false;
  std::string name;
  std::string version;         // current installed-file version
  std::string newest_version;  // newest file version on Nexus
  std::string category_id;     // Nexus category id
  std::string description;     // Nexus BBCode description
  std::string author;
};

class Provider : public Interface {
public:
  std::string source_type() const override { return "nexus"; }
  bool fetch(const ::engine::Mod &mod, ::engine::PipelineContext &ctx,
             const std::filesystem::path &dest_path) override;
  // Returns the real Nexus file metadata (files/{file}.json): the archive
  // name (file_name) for correct naming/extension and the display name.
  SourceDownloadInfo resolve_download_info(const ::engine::Mod &mod) const override;
  std::string display_name() const override;
  // The Nexus API budget: hourly and daily requests left, out of their limits
  // (the same numbers Settings > Sources shows). "--" while logged out, when
  // there is no budget to report.
  SourceRateLimit rate_limit_readout() const override;

  // Live mod-info lookup for the Mod Info Nexus tab ("Refresh" button).
  // Requires a configured API key; fills ModInfoResult::available=false on
  // any failure. Never throws.
  ModInfoResult fetch_mod_info(const std::string &nexus_domain,
                               const std::string &mod_id) const;

  // Pure body parser for the mods/{game}/mods/{id}.json response (extracted
  // so the mapping is unit-testable without the network).
  static ModInfoResult parse_mod_info(const std::string &body);

  // The downloadable files of one mod, from
  // mods/{game}/mods/{id}/files.json. Nexus lists a file as available only
  // while it is neither archived nor deleted, so a pin that is missing from
  // here is a pin that can no longer be honoured.
  struct FileList {
    bool ok = false;
    std::vector<long long> available;
  };

  // Pure parser for the response above. Never throws; ok=false on anything
  // unparseable or empty.
  static FileList parse_file_list(const std::string &body);

  // Which file to ask Nexus for, given the pack's declared policy. "exact"
  // takes the pin or nothing - it has no fallback to offer. "prefer" takes the
  // pin while it is still available and the mod's newest file once it is not,
  // which is the whole point of that state. "latest" always takes the newest.
  // An unknown policy is treated as exact. Returns 0 when nothing is available,
  // and the caller reports the failure rather than downloading a wrong file.
  static long long select_file_id(std::string_view policy, long long pinned,
                                  const FileList &list);

private:
  // Live mods/{game}/mods/{id}/files.json for one mod. ok=false on any
  // failure, which callers surface instead of guessing a file.
  FileList fetch_file_list(const std::string &nexus_domain,
                           const std::string &mod_id) const;

  // Shared download routine for a resolved URL (used by both the API-auth
  // paths and the direct-URL path). server_name is the Nexus mirror that
  // served the URL (from the download_link.json entry); when non-empty a
  // speed sample is recorded against it on success (MO2 parity).
  bool download_from_url(const std::string &url, ::engine::PipelineContext &ctx,
                         const std::filesystem::path &dest_path,
                         const std::string &server_name = {});
};

}  // namespace engine::Source::Nexus
