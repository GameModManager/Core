#include "engine/pipeline/fetch_stage.h"
#include "engine/pipeline/pipeline.h"
#include "engine/mod/model/mod.h"
#include "engine/source/registry.h"
#include "engine/instance/instance.h"
#include "engine/log/logger.h"
#include "engine/source/update_policy.h"
#include "engine/uri/modl/provider.h"

#include <sstream>

namespace engine {

Source::Interface *fetch_provider_for(const Mod &mod) {
  if (auto *provider =
          Source::Registry::instance().provider_for(mod.download_source_type))
    return provider;
  // No source, but the URL is already resolved: the transport takes it from
  // here. modl:// is the case that matters - it is a URI handler, so a link
  // from an unrecognized host carries no provenance and the mod stays
  // "manual" while the bytes still come down.
  if (!mod.download_url.empty())
    return &Uri::Modl::transport();
  return nullptr;
}

Source::VerifyResult verify_downloaded_archive(const Mod &mod,
                                               const std::filesystem::path &archive) {
  // A "latest" policy is the pack saying "whatever the source has now", so
  // there is deliberately nothing to check against.
  if (mod.download_nxm.update_policy == "latest") {
    Source::VerifyResult r;
    r.verdict = Source::PinVerdict::NoPin;
    r.message = "latest policy - the pack pinned no digest";
    return r;
  }

  Source::SourcePin pin;
  pin.sha256    = mod.expected_sha256;
  pin.file_size = mod.expected_file_size;
  pin.version   = mod.version;

  Source::ResolvedIdentity got;
  got.sha256 = Source::compute_file_sha256(archive.string());
  std::error_code ec;
  const auto size = std::filesystem::file_size(archive, ec);
  if (!ec)
    got.file_size = static_cast<int64_t>(size);

  return Source::verify_resolved(pin, got);
}

bool FetchStage::execute(Mod &mod, PipelineContext &ctx) {
  Logger::instance().debug("[FetchStage] execute: id=" + mod.id +
                           " source_type=" + mod.download_source_type +
                           " source_id=" + mod.download_source_id);
  // No download info → nothing to fetch
  if (mod.download_source_type.empty()) {
    Logger::instance().debug(
        "[FetchStage] No download_source_type, marking as Downloaded");
    mod.state = ModState::Downloaded;
    return true;
  }

  // Find the provider for this source, or the transport for a mod that
  // already resolved its own download URL.
  auto *provider = fetch_provider_for(mod);
  if (!provider) {
    Logger::instance().error("[FetchStage] No provider for source type '" +
                             mod.download_source_type + "'");
    ctx.error_message = "no download provider is registered for source type '" +
                        mod.download_source_type +
                        "' - GameModManager does not know how to download this mod";
    return false;
  }
  Logger::instance().debug("[FetchStage] Found provider: " + provider->display_name() +
                           " for source_type=" + mod.download_source_type);

  // Determine download destination
  std::filesystem::path dest_dir;
  if (ctx.instance) {
    dest_dir = ctx.instance->path_for(InstanceKind::Downloads);
  } else {
    dest_dir = ctx.mods_dir.parent_path() / "downloads";
  }
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);

  // Build archive filename. Providers may resolve the real file name (e.g.
  // Nexus file_name, with the correct extension); otherwise fall back to the
  // generic "<source_id>[-<file_id>].zip".
  std::ostringstream fname;
  auto info = provider->resolve_download_info(mod);
  if (!info.archive_name.empty()) {
    fname << info.archive_name;
  } else {
    fname << mod.download_source_id;
    if (mod.download_nxm.file_id > 0)
      fname << "-" << mod.download_nxm.file_id;
    fname << ".zip";
  }
  mod.archive_filename = fname.str();

  // Carry the real display name (e.g. "SkyUI") up to the UI. Only when the
  // provider actually resolved one - the PipelineWorker's placeholder
  // ("Mod file <id>") is left untouched otherwise.
  if (!info.display_name.empty())
    mod.name = info.display_name;

  // Surface the resolved metadata (archive name + display name) as soon as
  // it is known - before the download starts - so the UI can replace its
  // placeholder row name immediately rather than at download_complete.
  if (ctx.on_download_meta)
    ctx.on_download_meta(info.archive_name, info.display_name);

  auto dest_path = dest_dir / mod.archive_filename;

  // An in-progress download lands under "<name>.unfinished" and is renamed to
  // the final name only once every byte is in (MO2 downloadmanager.cpp:247,
  // :2475). Until then the file on disk is honest about being partial: a
  // crash, a kill, or an abort leaves "<name>.zip.unfinished", never a
  // truncated archive wearing the real name that the folder scanner and any
  // external archiver would happily pick up as a finished download.
  auto partial_path = dest_path;
  partial_path += ".unfinished";

  // Resume support: if a partial download already exists (a paused download
  // was aborted and kept its .unfinished file), continue from its size via
  // HTTP Range. The partial name is the only place a half-written archive can
  // live, so it is the only file whose size is a valid resume offset.
  ctx.download_resume_from = 0;
  if (std::filesystem::exists(partial_path, ec)) {
    auto sz = std::filesystem::file_size(partial_path, ec);
    if (!ec && sz > 0) {
      ctx.download_resume_from = static_cast<int64_t>(sz);
      Logger::instance().debug("[FetchStage] Resuming partial download of " +
                               mod.download_source_id + " at byte " +
                               std::to_string(ctx.download_resume_from));
    }
  }

  Logger::instance().debug(
      "[FetchStage] Starting download: " + mod.download_source_type + " mod " +
      mod.download_source_id + " to " + partial_path.string() +
      " resume_from=" + std::to_string(ctx.download_resume_from));

  if (!provider->fetch(mod, ctx, partial_path)) {
    Logger::instance().error("[FetchStage] Provider fetch returned false for " +
                             mod.id);
    // The provider reports its own cause to the log; the stage can only name
    // what it knows - which provider refused, what it was fetching, and where
    // the file would have landed.
    ctx.error_message = "the download from " + provider->display_name() + " failed (" +
                        mod.download_source_type + " mod " + mod.download_source_id +
                        " -> " + partial_path.string() +
                        ") - nothing was written there; see the log for the cause";
    return false;
  }

  // Some providers (e.g. SteamWorkshop) are metadata-only - no file produced
  if (!std::filesystem::exists(partial_path)) {
    mod.archive_filename.clear();
    mod.state = ModState::Downloaded;
    Logger::instance().debug("[FetchStage] Metadata updated, no archive file");
    return true;
  }

  // The bytes are on disk; check them against the digest the pack declared for
  // them. This is the only point in the install where a pin can be tested -
  // afterwards the archive is extracted and its identity is gone. A mismatch
  // stops the install: the file left behind is not the file the pack named,
  // and installing it would put unverified content in the instance.
  const Source::VerifyResult verified = verify_downloaded_archive(mod, partial_path);
  if (verified.verdict == Source::PinVerdict::Mismatch) {
    Logger::instance().error("[FetchStage] Digest mismatch for " + mod.id + ": " +
                             verified.message);
    // The partial is the known-bad payload, and keeping it buys nothing: the
    // next run reads its size as the HTTP Range offset, so a complete-but-wrong
    // file resumes from EOF, downloads zero bytes and fails the same way again
    // (the .unfinished orphan). MO2 never keeps a download it has judged bad
    // either - resumeDownloadInt sends no Range header for an errored download,
    // so it restarts from byte 0
    // (references/modorganizer/src/downloadmanager.cpp:1082-1089), and once
    // the retries are spent the partial is removed outright (:2429,
    // `info->m_Output.remove()`). Same rule here: discard and refetch. The
    // staged bytes have already proved they are not this file, so there is
    // nothing in them worth a metered re-download.
    std::filesystem::remove(partial_path, ec);
    if (ec)
      Logger::instance().error(
          "[FetchStage] Could not discard the mismatched partial " +
          partial_path.string() + ": " + ec.message());
    ctx.error_message = "the downloaded archive is not the file this pack asked for (" +
                        verified.message +
                        "). Nothing was installed; the bad download was discarded, so "
                        "starting it again fetches it from scratch";
    return false;
  }
  Logger::instance().debug("[FetchStage] Download verification: " + verified.message);

  // Every byte is in and the bytes are the ones we wanted: drop the
  // ".unfinished" marker by renaming onto the final archive name. This is the
  // same "don't rename but remove the .unfinished extension" step MO2 does on
  // completion (downloadmanager.cpp:2475). From here on the file on disk is a
  // finished archive under its real name.
  std::filesystem::rename(partial_path, dest_path, ec);
  if (ec) {
    Logger::instance().error("[FetchStage] Failed to finalize download for " + mod.id +
                             ": " + ec.message());
    ctx.error_message = std::string("the download finished but the completed archive "
                                    "could not be renamed to ") +
                        dest_path.string() + " (it is still at " +
                        partial_path.string() + ")";
    return false;
  }

  // Add archive to mod files for subsequent stages
  ModFile mf;
  mf.relative_path = dest_path.string();
  mod.files.push_back(mf);

  mod.state = ModState::Downloaded;
  Logger::instance().debug("[FetchStage] Download complete: " + mod.id + " -> " +
                           dest_path.string());
  return true;
}

}  // namespace engine
