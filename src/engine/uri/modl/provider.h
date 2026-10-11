#pragma once

#include "engine/source/interface.h"

#include <string>

namespace engine::Uri::Modl {

// Transport for the modl:// protocol, in the same family as nxm://: a URI
// handler, not a provenance. It downloads a plain https URL the caller
// already resolved and has no source to attribute the bytes to - the source
// is derived from that URL's host by Router::derive_source_from_direct_url
// and stamped on the mod before the pipeline runs (mod.pub -> "modpub",
// anything else -> "manual").
//
// Deliberately NOT registered in the Source::Registry: a registry entry is a
// user-attributable source, and this carries no provenance, no account and
// no settings. transport() hands the object to FetchStage directly for a mod
// that already holds a resolved download URL, which is a transport question
// (where do the bytes come from) rather than a source one.
//
// source_type() is a transport identifier, never a Mod::download_source_type.
//
// This is the ONE crossing between the two sides, and it is a declared one:
// Provider derives from engine::Source::Interface, the abstract five-pure-
// virtual base, and nothing else. It never includes a concrete provider, so
// a modl:// handler cannot read a Nexus API key or a Steam session.
class Provider : public Source::Interface {
public:
  std::string source_type() const override { return "direct"; }
  bool fetch(const ::engine::Mod &mod, ::engine::PipelineContext &ctx,
             const std::filesystem::path &dest_path) override;
  Source::SourceDownloadInfo
  resolve_download_info(const ::engine::Mod &mod) const override;
  std::string display_name() const override;
};

// The single shared transport instance. Stateless, so one is enough.
[[nodiscard]] Source::Interface &transport();

}  // namespace engine::Uri::Modl
