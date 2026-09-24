#pragma once

// Pack source detection - classify a user-supplied pack reference BEFORE any
// adapter parses it.
//
// A pack is either file-based (a .gmmpack zip archive on disk) or API-based
// (a Nexus collection reached through an nxm:// URL or a
// nexusmods.com/collections/ page URL - never as a raw file). Detection runs
// cheap string probes first (scheme, host, extension) and touches the
// filesystem only as a last resort (content sniffing), so classifying a URL
// never stats the disk.
//
// The result carries the adapter format id ("gmmpack" / "nexus-collection")
// so the installer can route to the adapter that handles it. This header
// deliberately does not depend on the adapter interface (which lives in a
// parallel change) - the ids are documented literals kept in sync by test.
//
// To add a format: extend PackFormat, add its id below, and append a probe
// to kProbes in the .cpp. Probes run in order, first decisive one wins.
//
// Engine layer - Qt-free (only <string>, <string_view>).

#include <string>
#include <string_view>

namespace engine::Pack {

// Pack formats the installer knows how to route. Unknown means "no adapter
// handles this reference" - the caller should surface Detection::reason.
enum class PackFormat {
  Unknown,
  Gmmpack,          // local .gmmpack zip archive (manifest.json at root)
  NexusCollection,  // Nexus collection via nxm:// or nexusmods.com/collections/
};

// Adapter registry id for a format: "gmmpack", "nexus-collection", or ""
// for Unknown. Matches Interface::format_id() in engine/pack/adapter.h.
[[nodiscard]] std::string_view format_id_of(PackFormat format);

struct Detection {
  PackFormat format = PackFormat::Unknown;
  std::string format_id;  // adapter id, "" when Unknown
  std::string reason;     // human-readable: why this format / why unknown

  [[nodiscard]] bool known() const { return format != PackFormat::Unknown; }
};

// Classify url_or_path (local path, direct-download URL, nxm:// link, or
// nexusmods.com page URL). Never throws; unrecognized input yields Unknown
// with a reason suitable for logs and UI errors.
[[nodiscard]] Detection detect_pack_source(const std::string &url_or_path);

}  // namespace engine::Pack
