#include "engine/pipeline/install_stage.h"
#include "engine/core/instance/instance.h"
#include "engine/core/log/logger.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>

namespace engine {

namespace Source {

// Copy a directory tree. When `on_progress` is set, it is called with the
// percent of files copied (0-100). Files are counted in a cheap first pass so
// the bar shows a real percent instead of a spinner; a tree that cannot be
// enumerated reports -1 (indeterminate) for each copied file.
static bool
copy_recursive(const std::filesystem::path &src,
               const std::filesystem::path &dst,
               const std::function<void(int percent)> &on_progress = {}) {
  std::error_code ec;
  std::filesystem::create_directories(dst, ec);
  if (ec)
    return false;

  int64_t total = 0;
  for (auto it = std::filesystem::recursive_directory_iterator(
           src, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (it->is_regular_file())
      ++total;
  }

  int64_t done = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           src, std::filesystem::directory_options::skip_permission_denied)) {
    auto relative = std::filesystem::relative(entry.path(), src);
    auto dest_path = dst / relative;

    if (entry.is_directory()) {
      std::filesystem::create_directories(dest_path, ec);
      if (ec)
        return false;
    } else if (entry.is_regular_file()) {
      std::filesystem::copy(entry.path(), dest_path,
                            std::filesystem::copy_options::overwrite_existing,
                            ec);
      if (ec)
        return false;
      ++done;
      if (on_progress) {
        const int pct = total > 0 ? static_cast<int>(done * 100 / total) : -1;
        on_progress(std::clamp(pct, 0, 100));
      }
    }
  }
  return true;
}

// MO2's generateBackupName: "<name>_backup", "<name>_backup1", ... picking the
// first name that does not already exist.
static std::filesystem::path
generate_backup_name(const std::filesystem::path &dir) {
  auto backup = dir.string() + "_backup";
  if (!std::filesystem::exists(backup))
    return backup;
  for (int i = 1;; ++i) {
    auto candidate = dir.string() + "_backup" + std::to_string(i);
    if (!std::filesystem::exists(candidate))
      return candidate;
  }
}

bool InstallationManager::execute(Mod &mod, PipelineContext &ctx) {
  // Find staging directory from the mod's files (set by ExtractStage)
  std::filesystem::path staging_dir;
  for (const auto &f : mod.files) {
    auto p = std::filesystem::path(f.relative_path);
    if (std::filesystem::exists(p / "metadata.xml") ||
        std::filesystem::is_directory(p)) {
      staging_dir = p;
      break;
    }
  }

  if (staging_dir.empty()) {
    // Metadata-only mod (e.g. Steam Workshop already on disk) - nothing to
    // install
    if (mod.state == ModState::Extracted) {
      mod.state = ModState::Installed;
      return true;
    }
    Logger::instance().error("InstallStage: no staging directory found");
    return false;
  }

  // Non-FOMOD installs get a name-confirmation step (MO2's
  // SimpleInstallDialog): the UI pre-fills its best guess (typically the Nexus
  // display name) and offers the archive filename in a dropdown. FOMOD archives
  // are skipped - their wizard already owns the name. Canceling aborts the
  // whole install (not a failure - the download keeps its state).
  if (ctx.name_query_cb && !ctx.fomod_detected) {
    auto chosen = ctx.name_query_cb(mod.name, mod.archive_filename);
    if (!chosen) {
      Logger::instance().debug("InstallStage: install canceled in name dialog");
      ctx.canceled = true;
      return false;
    }
    mod.name = *chosen;
  }

  // Determine mod folder name - the display name (e.g. "SkyUI") is the
  // MO2-style folder name. The download id (mod_id-file_id) is only a
  // fallback for sources that never resolved a display name.
  std::string folder_name = mod.name;
  if (folder_name.empty()) {
    folder_name = mod.id;
  }
  if (folder_name.empty()) {
    folder_name = mod.download_source_id;
  }
  if (folder_name.empty()) {
    Logger::instance().error("InstallStage: cannot determine mod folder name");
    return false;
  }

  // Sanitize folder name (replace problematic characters)
  for (auto &c : folder_name) {
    if (c == '/' || c == '\\' || c == '\0')
      c = '_';
  }

  // Destination in mods/
  auto mods_dir = ctx.mods_dir;
  if (mods_dir.empty() && ctx.instance) {
    mods_dir = ctx.instance->path_for(InstanceKind::Mods);
  }
  if (mods_dir.empty()) {
    Logger::instance().error("InstallStage: no mods directory in context");
    return false;
  }

  // Ask the user how to proceed when the mod folder already exists (MO2's
  // testOverwrite in installationmanager.cpp): Merge adds files into the
  // existing folder, Replace deletes it and installs fresh, Rename installs
  // under a new folder name (re-checked in a loop, since the new name may
  // also exist), Cancel aborts. Without a callback the headless default is a
  // silent replace (the behavior before the query dialog existed).
  auto dest_dir = mods_dir / folder_name;
  while (std::filesystem::exists(dest_dir)) {
    if (!ctx.overwrite_query_cb) {
      Logger::instance().warn(
          "InstallStage: mod folder already exists, removing: " +
          dest_dir.string());
      std::error_code ec;
      std::filesystem::remove_all(dest_dir, ec);
      break;
    }

    auto decision = ctx.overwrite_query_cb(folder_name);
    if (decision.action == OverwriteAction::Cancel) {
      Logger::instance().debug("InstallStage: install canceled by user");
      ctx.canceled = true;
      return false;
    }

    if (decision.backup) {
      auto backup_dir = generate_backup_name(dest_dir);
      Logger::instance().debug("InstallStage: backing up " + dest_dir.string() +
                               " to " + backup_dir.string());
      if (!copy_recursive(dest_dir, backup_dir)) {
        Logger::instance().error("InstallStage: failed to create backup " +
                                 backup_dir.string());
        return false;
      }
    }

    if (decision.action == OverwriteAction::Rename) {
      folder_name = decision.new_name;
      for (auto &c : folder_name) {
        if (c == '/' || c == '\\' || c == '\0')
          c = '_';
      }
      if (folder_name.empty()) {
        Logger::instance().error(
            "InstallStage: rename produced an empty folder name");
        return false;
      }
      dest_dir = mods_dir / folder_name;
      continue; // re-check: the new name may also exist
    }

    if (decision.action == OverwriteAction::Replace) {
      Logger::instance().warn("InstallStage: replacing existing mod folder " +
                              dest_dir.string());
      std::error_code ec;
      std::filesystem::remove_all(dest_dir, ec);
    }

    // Merge (existing folder kept) or Replace (fresh empty folder) both
    // fall through to the copy below.
    break;
  }

  // The folder name is now final (a Rename decision may have replaced it
  // above). Record it so the caller can add just this one mod to the list.
  ctx.installed_mod_folder = folder_name;

  // Copy extracted files to mods/{folder_name}/
  Logger::instance().debug("InstallStage: installing to " + dest_dir.string());
  const std::string install_status = "Installing to " + folder_name + "…";
  if (!copy_recursive(staging_dir, dest_dir,
                      [&ctx, &install_status](int percent) {
                        if (ctx.on_stage_progress)
                          ctx.on_stage_progress(percent, install_status);
                      })) {
    Logger::instance().error("InstallStage: failed to copy files to " +
                             dest_dir.string());
    return false;
  }

  // Ensure the game's metadata file exists in the mod folder so ModScanner
  // can find it. MO2-style games get a meta.ini (MO2's installers write the
  // same file with the same keys); metadata.xml is an Isaac-only trick - the
  // Isaac engine reads it from mod folders directly - and is written only
  // for games that registered the metadata_file hook.
  std::string display_name = mod.name.empty() ? folder_name : mod.name;
  ModMeta::write_game_metadata(dest_dir, ctx.metadata_file, display_name,
                               mod.version, mod.download_source_id,
                               mod.archive_filename);

  // FOMOD choice persistence: record the installer's selections in the mod
  // folder's meta.ini (MO2-style games only - Isaac's metadata.xml is the
  // game's own format and must not be extended) so a reinstall can restore
  // them, and Phase B's scanner can flag FOMOD-installed mods.
  if ((ctx.metadata_file.empty() || ctx.metadata_file == "meta.ini") &&
      !ctx.fomod_choices_json.empty()) {
    const auto fomod_meta_path = dest_dir / "meta.ini";
    std::ifstream fmod(fomod_meta_path);
    std::string fmod_content((std::istreambuf_iterator<char>(fmod)),
                             std::istreambuf_iterator<char>());
    ModMeta fmod_meta;
    if (fmod_content.empty() || fmod_meta.parse(fmod_content)) {
      fmod_meta.set("fomod", "choices", ctx.fomod_choices_json);
      std::ofstream(fomod_meta_path) << fmod_meta.serialize();
    }
  }

  // Clean up staging directory
  std::error_code ec;
  std::filesystem::remove_all(staging_dir, ec);

  // Write meta.ini
  auto meta_dir = ctx.meta_dir;
  if (meta_dir.empty() && ctx.instance) {
    meta_dir = ctx.instance->path_for(InstanceKind::Meta);
  }

  if (!meta_dir.empty()) {
    auto meta = ModMeta::from_default(
        folder_name,
        mod.download_source_type.empty() ? "manual" : mod.download_source_type,
        mod.download_source_id, mod.archive_filename, mod.version);

    // Reinstall: preserve the previously persisted priority so the mod keeps
    // its position in the load order instead of resetting to the top.
    auto existing = ModMeta::load(meta_dir, folder_name);
    if (existing.priority() >= 0) {
      meta.set_priority(existing.priority());
    }

    // For Nexus downloads, add [Nexusmods] section
    if (mod.download_source_type == "nexus" && mod.download_nxm.file_id > 0) {
      meta.set("Nexusmods", "modid", mod.download_source_id);
      meta.set("Nexusmods", "fileid", std::to_string(mod.download_nxm.file_id));
    }

    // LoversLab has no API, so its provenance lives in the [LoversLab]
    // section: the file id, the page URL (the download link minus the
    // ?do=download query - the slug cannot be reconstructed from the id),
    // and what the download itself resolved to.
    if (mod.download_source_type == "loverslab") {
      if (!mod.download_source_id.empty())
        meta.set("LoversLab", "fileid", mod.download_source_id);
      if (!mod.download_page_url.empty())
        meta.set("LoversLab", "page_url", mod.download_page_url);
      if (!mod.name.empty())
        meta.set("LoversLab", "display_name", mod.name);
      if (!mod.archive_filename.empty())
        meta.set("LoversLab", "archive_filename", mod.archive_filename);
    }

    if (!meta.save(meta_dir, folder_name)) {
      Logger::instance().warn("InstallStage: failed to write meta.ini for " +
                              folder_name);
    }
  }

  mod.id = folder_name;
  mod.state = ModState::Installed;
  Logger::instance().debug("InstallStage: installed " + folder_name);
  return true;
}

} // namespace Source

} // namespace engine
