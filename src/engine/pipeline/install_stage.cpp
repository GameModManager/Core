#include "engine/pipeline/install_stage.h"
#include "engine/instance/instance.h"
#include "engine/log/logger.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/source/modpub/provider.h"
#include "engine/source/nexus/update_check.h"
#include "engine/source/update_policy.h"
#include "engine/util/fs_utils.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <utility>

namespace engine {

namespace Source {

  // Copy a directory tree. When `on_progress` is set, it is called with the
  // percent of files copied (0-100). Files are counted in a cheap first pass so
  // the bar shows a real percent instead of a spinner; a tree that cannot be
  // enumerated reports -1 (indeterminate) for each copied file.
  static bool copy_recursive(const std::filesystem::path &src,
                             const std::filesystem::path &dst,
                             const std::function<void(int percent)> &on_progress = {}) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    if (ec)
      return false;

    int64_t total = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(
             src, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
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
      auto relative  = std::filesystem::relative(entry.path(), src);
      auto dest_path = dst / relative;

      if (entry.is_directory()) {
        std::filesystem::create_directories(dest_path, ec);
        if (ec)
          return false;
      } else if (entry.is_regular_file()) {
        std::filesystem::copy(entry.path(), dest_path,
                              std::filesystem::copy_options::overwrite_existing, ec);
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

  // The keys in a mod folder's meta.ini that describe the ARCHIVE that was
  // installed, not the MOD ROW. Everything else in [General] and
  // [GameModManager] describes the row and has to survive an overwrite,
  // because nothing below re-derives it - the archive is new, the mod is the
  // same mod.
  //
  // This list is the whole fix for the whack-a-mole. The carry used to be a
  // hand-written pair of `replaced_*` variables per key, which is how
  // rootOverride (deploy_utils.cpp:413 reads it and deploys the mod to the GAME
  // ROOT), validated, color and category each shipped a release dropping on
  // every overwrite before anyone noticed. A key nobody has thought about yet
  // now survives by default; opting a NEW key out is the deliberate act.
  //
  // Every entry below is either re-stamped from the install manifest further
  // down, or is exactly the identity of the archive that is being installed -
  // carrying it would name the file that is no longer on disk. "modid" is the
  // sharp one: it is the Nexus id namespace (mod_meta.cpp write_game_metadata),
  // so carrying an old Nexus id onto a Steam or LoversLab install mis-attributes
  // the mod's provenance to MO2 and to every reader that trusts it.
  //
  // Deliberately NOT carried, and why:
  //   [Git]        - a commit id for content that is no longer that commit. A
  //                  Replace of a git-tracked folder with an archive makes the
  //                  folder no longer a checkout, so the badge would lie.
  //   [Nexusmods], [LoversLab], [ModPub], [SteamWorkshop], [fomod] - per-install
  //                  provenance. The sections for the NEW source are written
  //                  below; carrying the old ones would attach a stale file id
  //                  to a mod from a different provider, and a stale FOMOD
  //                  choice set would flag a non-FOMOD archive as FOMOD-installed.
  static const std::set<std::pair<std::string, std::string>> per_install_meta_keys = {
      {"General", "modid"},
      {"General", "version"},
      {"General", "installationFile"},
      {"General", "installationfile"},
      {"General", "installed"},
      // The update verdict, both spellings of each. Carried explicitly below,
      // with the rules that go with them (reconcile_update_symbol reads the
      // lowercase spelling first, so a blanket copy could leave the two
      // casings disagreeing). nexus_update is Nexus's key, update_available
      // the page-scrape sources'; each source only ever writes its own, so
      // carrying both is a no-op for the other source.
      {"General", "nexus_update"},
      {"General", "update_available"},
      {"General", "newestversion"},
      {"General", "newestVersion"},
      {"General", "ignoredVersion"},
      {"General", "ignoredversion"},
      // Re-derived from the install manifest below (from_default, and the
      // download size/hash block).
      {"GameModManager", "folder"},
      {"GameModManager", "source_type"},
      {"GameModManager", "source_id"},
      {"GameModManager", "imported_from_mo2"},
      {"GameModManager", "original_meta_path"},
      {"GameModManager", "meta_version"},
      {"GameModManager", "download_size"},
      {"GameModManager", "download_sha256"},
      // The mod's place in the list, carried explicitly below.
      {"GameModManager", "priority"},
      {"GameModManager", "separator_id"},
  };

  // MO2's generateBackupName: "<name>_backup", "<name>_backup1", ... picking the
  // first name that does not already exist.
  static std::filesystem::path generate_backup_name(const std::filesystem::path &dir) {
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
      ctx.error_message =
          "nothing was extracted from " +
          (mod.archive_filename.empty() ? std::string("the archive")
                                        : mod.archive_filename) +
          ", so there are no files to install";
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
      ctx.error_message = "the mod has no name, id or download id, so GameModManager "
                          "cannot tell which folder to install it into";
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
      ctx.error_message = "this instance has no mods directory configured, so "
                          "there is nowhere to install the mod";
      return false;
    }

    // Ask the user how to proceed when the mod folder already exists (MO2's
    // testOverwrite in installationmanager.cpp): Merge adds files into the
    // existing folder, Replace deletes it and installs fresh, Rename installs
    // under a new folder name (re-checked in a loop, since the new name may
    // also exist), Cancel aborts. Without a callback the headless default is a
    // silent replace (the behavior before the query dialog existed).

    // Load-order state a Replace would otherwise destroy, carried out of the
    // folder before it is deleted and read back in when the new meta.ini is
    // stamped below. MO2 saves the whole of meta.ini around the delete and
    // restores it (installationmanager.cpp:417-447); we only need the keys
    // the install path below does not write itself.
    int replaced_priority = -1;
    std::string replaced_separator;
    // The Nexus update verdict the sweep wrote, for the same reason and in the
    // same set. write_game_metadata early-returns when meta.ini exists and
    // otherwise CREATES one holding five keys (mod_meta.cpp:804-838), so on a
    // Replace nexus_update / newestversion / ignoredversion die with the folder
    // - and the update symbol then has nothing to be cleared FROM:
    // reconcile_update_symbol's first gate is nexus_update != "true"
    // (downloads_controller.cpp:1334), the row keeps wearing the badge it was
    // built with at startup, and no later install can ever take it off. That
    // is the user's "still flagged after updating to the newest" report.
    std::string replaced_nexus_update;
    std::string replaced_newest;
    std::string replaced_ignored;
    // Whether the folder carried the game's disable sentinel when the delete
    // took it. The sentinel is what the DEPLOY filter consults and the only
    // record of the mod being OFF that lives inside the folder
    // (deploy_utils.cpp:389), and nothing re-creates it: the profile's
    // modlist.txt holds the same verdict per profile, but deploy never reads
    // it. Without this a mod the user deliberately disabled is re-enabled -
    // in the row, in the deploy set, and therefore in the load order - by
    // nothing more than updating it.
    bool replaced_disabled = false;
    // Which categories the user put this mod in. Held as its own variable, not
    // left to the wholesale carry below, because write_game_metadata writes
    // category=0 into every meta it creates and the carry's "only if the new
    // meta does not hold the key" rule would read that default as an answer.
    std::string replaced_categories;
    // The mod's own meta.ini as the folder held it, whole, held in memory until
    // the new one is stamped. This is the general form of every per-key carry
    // below: MO2 saves the whole file around the delete and restores it
    // (installationmanager.cpp:417-447), and the only reason we cannot do
    // exactly that is per_install_meta_keys above. The named carries stay
    // because each has a rule the wholesale copy does not have - priority
    // survives only as a number, the archive is read for one field, the
    // sentinel is a FILE and not a key at all.
    ModMeta replaced_meta;

    auto dest_dir = mods_dir / folder_name;
    std::error_code ec_probe;
    // Everything a delete below would take with it, read out ONCE and before
    // either delete. Two deletes reach this point: the Replace decision, and the
    // headless silent replace taken when no overwrite callback is wired (the
    // CLI). MO2 only has the first, because it always asks
    // (installationmanager.cpp:387-465) - so the headless path used to lose
    // every key here too, which is a state-loss bug wearing a different hat.
    auto capture_replaced_state = [&](const std::filesystem::path &folder) {
      // Read the archive the old install came from before the delete takes
      // meta.ini with it, so the UI can stop claiming that archive is still
      // installed. MO2 reads the same field off ModInfo just before it
      // removes the directory (installationmanager.cpp:418-424).
      const auto old_meta           = ModMeta::load_file(folder / "meta.ini");
      replaced_meta                 = old_meta;
      replaced_categories           = old_meta.get("General", "category");
      const std::string old_archive = old_meta.get("General", "installationfile");
      if (!old_archive.empty())
        ctx.replaced_archive = old_archive;
      // The same read carries the mod's place in the list. meta.ini dies
      // with the folder, and nothing below re-derives either key, so without
      // this the reinstalled mod loses its load-order position and drops out
      // of its separator (from_default never writes either key, and
      // priority() reads -1 for an absent one).
      replaced_priority  = old_meta.priority();
      replaced_separator = old_meta.get("GameModManager", "separator_id");
      // Both spellings of the version the sweep named newest, for the same
      // reason reconcile_update_symbol reads both: kKeyNewest is the sweep's
      // lowercase spelling and "newestVersion" the install-stage one.
      replaced_nexus_update = old_meta.get("General", "nexus_update");
      replaced_newest       = old_meta.get("General", "newestversion");
      if (replaced_newest.empty())
        replaced_newest = old_meta.get("General", "newestVersion");
      replaced_ignored =
          engine::Source::Nexus::UpdateCheck::read_ignored_version(old_meta);
      if (!ctx.disable_mechanism.empty())
        replaced_disabled =
            std::filesystem::exists(folder / ctx.disable_mechanism, ec_probe);
    };

    while (std::filesystem::exists(dest_dir)) {
      if (!ctx.overwrite_query_cb) {
        Logger::instance().warn("InstallStage: mod folder already exists, removing: " +
                                dest_dir.string());
        capture_replaced_state(dest_dir);
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
          ctx.error_message = "could not back up the existing mod folder " +
                              dest_dir.string() + " to " + backup_dir.string() +
                              " - the existing install was left untouched";
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
          ctx.error_message = "the new mod name from the overwrite dialog is empty, "
                              "so there is no folder to install into";
          return false;
        }
        dest_dir = mods_dir / folder_name;
        continue;  // re-check: the new name may also exist
      }

      if (decision.action == OverwriteAction::Replace) {
        Logger::instance().warn("InstallStage: replacing existing mod folder " +
                                dest_dir.string());
        capture_replaced_state(dest_dir);
        std::error_code ec;
        std::filesystem::remove_all(dest_dir, ec);
      }

      // Merge (existing folder kept) or Replace (fresh empty folder) both
      // fall through to the copy below. Merge keeps the folder, so everything
      // read out of it above stays where it is - the carry below is a no-op
      // there and the sentinel, the priority and the update verdict all
      // survive untouched by not having been touched.
      break;
    }

    // The folder name is now final (a Rename decision may have replaced it
    // above). Record it so the caller can add just this one mod to the list.
    ctx.installed_mod_folder = folder_name;

    // Copy extracted files to mods/{folder_name}/
    Logger::instance().debug("InstallStage: installing to " + dest_dir.string());
    const std::string install_status = "Installing to " + folder_name + "…";
    if (!copy_recursive(staging_dir, dest_dir, [&ctx, &install_status](int percent) {
          if (ctx.on_stage_progress)
            ctx.on_stage_progress(percent, install_status);
        })) {
      Logger::instance().error("InstallStage: failed to copy files to " +
                               dest_dir.string());
      ctx.error_message = "could not copy the extracted files into " +
                          dest_dir.string() +
                          " - the mod folder is incomplete; check the mods "
                          "directory is writable and has free space";
      return false;
    }

    // Put the disable sentinel back, if the folder we deleted had one. The
    // user's deliberate "off" is not a property of the CONTENT being updated -
    // it is a property of the mod, and updating a mod must not be a way to
    // turn it back on. Written before the metadata block below so a failure
    // here still leaves the mod disabled rather than half-restored.
    if (replaced_disabled && !ctx.disable_mechanism.empty()) {
      std::error_code ec;
      if (create_empty_file(dest_dir / ctx.disable_mechanism, ec)) {
        Logger::instance().debug(
            "InstallStage: '" + folder_name +
            "' was disabled before the update and stays disabled (" +
            ctx.disable_mechanism + " carried across)");
      } else {
        Logger::instance().warn(
            "InstallStage: could not restore " + ctx.disable_mechanism + " for '" +
            folder_name + "': " + ec.message() +
            " - the mod will be ENABLED until it is switched off again");
      }
    }

    // Ensure the game's metadata file exists in the mod folder so ModScanner
    // can find it. MO2-style games get a meta.ini (MO2's installers write the
    // same file with the same keys); metadata.xml is an Isaac-only trick - the
    // Isaac engine reads it from mod folders directly - and is written only
    // for games that registered the metadata_file hook.
    //
    // The MO2 meta.ini [General] modid= field is the Nexus id namespace ONLY.
    // write_game_metadata takes the Nexus mod id (download_source_id when the
    // source is Nexus, empty otherwise) so a Steam workshop id or LoversLab
    // file id does NOT get written into meta.ini as a fake Nexus mod id and
    // mis-attribute the mod's provenance to MO2 / downstream readers.
    std::string display_name = mod.name.empty() ? folder_name : mod.name;
    const std::string nexus_mod_id =
        (mod.download_source_type == "nexus") ? mod.download_source_id : "";
    ModMeta::write_game_metadata(dest_dir, ctx.metadata_file, display_name, mod.version,
                                 nexus_mod_id, mod.archive_filename);

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

    // Write manager metadata into the mod folder's own meta.ini
    // (MO2-compatible in-folder location). The file already exists here:
    // write_game_metadata created it above (or the merged folder kept its
    // old one on Merge). Load it, stamp the manager keys on top, and save
    // back - a single-file merge, never a second sidecar file.
    if (!mods_dir.empty()) {
      auto meta_path = mods_dir / folder_name / "meta.ini";
      auto meta      = ModMeta::load_file(meta_path);

      // Reinstall (Merge): keep the persisted priority so the mod holds its
      // load-order position instead of resetting to the top. A Replace has no
      // meta.ini left to read, so the values carried out of the deleted folder
      // stand in - without them the branch below can never fire on Replace and
      // the mod reappears at the bottom of the user band.
      const int existing_priority =
          meta.priority() >= 0 ? meta.priority() : replaced_priority;

      auto fresh = ModMeta::from_default(
          folder_name,
          mod.download_source_type.empty() ? "manual" : mod.download_source_type,
          mod.download_source_id, mod.archive_filename, mod.version);
      // Which archive does the meta we are holding DESCRIBE? Read before the
      // refresh loop below overwrites the key, and from both spellings:
      // write_game_metadata writes installationFile (mod_meta.cpp) and
      // from_default writes installationfile.
      std::string meta_names = meta.get("General", "installationfile");
      if (meta_names.empty())
        meta_names = meta.get("General", "installationFile");
      for (const auto &key : fresh.keys("GameModManager"))
        meta.set("GameModManager", key, fresh.get("GameModManager", key));
      // Reinstall/merge must refresh the version stamp from the install
      // manifest: write_game_metadata early-returns when meta.ini exists
      // (always true on Merge), so without this the version column goes stale
      // after an update (Workspace-pmrh M3). Both casings ride along:
      // from_default writes installationfile, write_game_metadata writes
      // installationFile.
      for (const auto &key : fresh.keys("General"))
        if (key == "installed" || key == "version" || key == "installationfile")
          meta.set("General", key, fresh.get("General", key));
      // When the install supplied NO version, the loop above never visits
      // "version" (from_default writes the key only for a non-empty version,
      // mod_meta.cpp) and the meta.ini still on disk can be the PREVIOUS
      // install's. That is not a stale cache, it is a confident wrong answer:
      // the Version column shows the version of content that is no longer the
      // whole of what is installed. It is also self-sealing, because
      // record_installed_version skips the Nexus lookup whenever that value is
      // non-empty - so the wrong value could never be corrected by anything.
      //
      // Cleared only when the meta on disk names a DIFFERENT archive, which is
      // the one case that proves the value is a survivor rather than this
      // install's own. That exclusion cuts both ways, and both are the same bug
      // in a different coat: a Replace reads the meta the copy just put there,
      // and a Merge of an archive that ships its own meta.ini reads that one -
      // both name mod.archive_filename, and wiping their version would invent a
      // new wrong answer. A reinstall of the SAME archive keeps its version,
      // which is true (it is the same content) and spends no Nexus request.
      if (mod.version.empty() && !meta_names.empty() &&
          meta_names != mod.archive_filename)
        meta.set("General", "version", "");
      // newestVersion + installationFile are NOT in fresh (from_default never
      // writes them - they come from write_game_metadata on fresh installs),
      // so stamp them explicitly from the manifest or they go stale on Merge.
      if (!mod.version.empty())
        meta.set("General", "newestVersion", mod.version);
      if (!mod.archive_filename.empty())
        meta.set("General", "installationFile", mod.archive_filename);
      // The download's own identity, recorded once at install time. An exported
      // modpack pinning this mod with updatePolicy "exact" has to declare the
      // archive's size and sha256, and neither is recoverable from the
      // installed folder afterwards (that is exactly the state this used to
      // be in, which is why every exported pack came out "latest").
      if (!mod.files.empty()) {
        const std::string archive = mod.files.front().relative_path;
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(archive, size_ec);
        if (!size_ec && size > 0) {
          const std::string hash = Source::compute_file_sha256(archive);
          if (!hash.empty()) {
            meta.set("GameModManager", "download_size", std::to_string(size));
            meta.set("GameModManager", "download_sha256", hash);
          }
        }
      }

      // Everything else the row's own meta.ini held, copied across key by key.
      // A carried key is written only where the new meta does not already HAVE
      // that key, so a meta.ini that arrived inside the archive keeps what it
      // shipped - including a key it shipped deliberately empty, which is why
      // the test is key PRESENCE and not "value is empty".
      //
      // This is where rootOverride, validated and color live, and where the keys
      // nobody has named yet will live. Nothing above re-derives any of them:
      // the install knows the archive, not what the user did to the mod row.
      // Hybrid games (Isaac-style): only managed (non-Workshop) mods must
      // never carry rootOverride. Determine workshop status from the install
      // source and the meta on disk (old + new) plus Isaac's trailing-digit
      // folder-name convention.
      bool is_workshop =
          mod.download_source_type == "steam" ||
          mod.download_source_type == "steam_workshop" ||
          !meta.get("SteamWorkshop", "workshop_id").empty() ||
          !replaced_meta.get("SteamWorkshop", "workshop_id").empty() ||
          meta.get("GameModManager", "source_type") == "steam" ||
          meta.get("GameModManager", "source_type") == "steam_workshop" ||
          replaced_meta.get("GameModManager", "source_type") == "steam" ||
          replaced_meta.get("GameModManager", "source_type") == "steam_workshop";
      if (!is_workshop) {
        const std::string &fn = folder_name;
        auto us = fn.rfind('_');
        if (us != std::string::npos && us + 1 < fn.size()) {
          bool all_digits = true;
          for (size_t k = us + 1; k < fn.size(); ++k)
            if (!std::isdigit(static_cast<unsigned char>(fn[k]))) {
              all_digits = false;
              break;
            }
          if (all_digits)
            is_workshop = true;
        }
      }

      // Dropping rootOverride is the expensive one - deploy_utils reads
      // [General] rootOverride to send the mod to the GAME ROOT instead of the
      // data dir, so a mod flagged root-override quietly stopped reaching the
      // game after every single update.
      for (const auto &section : {std::string("General"), std::string("GameModManager"),
                                  std::string("Mirror")}) {
        const auto held = meta.keys(section);
        for (const auto &key : replaced_meta.keys(section)) {
          if (per_install_meta_keys.count({section, key}) != 0)
            continue;
          // Hybrid games (Isaac-style, deploy_include_mod_id): managed
          // (non-Workshop) mods deploy to their own folder, so rootOverride
          // must never survive an update - otherwise a managed mod scatters
          // into the mods root. Workshop mods keep a legitimate rootOverride.
          if (ctx.deploy_include_mod_id && section == "General" &&
              key == "rootOverride" && !is_workshop)
            continue;
          const bool already_there =
              std::find(held.begin(), held.end(), key) != held.end();
          const std::string value = replaced_meta.get(section, key);
          if (!already_there && !value.empty())
            meta.set(section, key, value);
        }
      }

      // Hybrid games (Isaac-style): a managed (non-Workshop) mod must never
      // carry a rootOverride flag, even one shipped inside the archive's own
      // meta.ini - managed mods deploy to their own folder, not the mods root.
      // Workshop mods keep a legitimate rootOverride.
      if (ctx.deploy_include_mod_id && !is_workshop)
        meta.unset("General", "rootOverride");

      // Filter membership, with the one carry that deliberately overrides the
      // new meta. write_game_metadata writes category=0 into every meta it
      // creates (mod_meta.cpp) and 0 means "no categories", which is a DEFAULT
      // the installer picked, not anything it learned about this mod - so the
      // presence rule above reads it as the archive having an opinion and
      // leaves the user's own categories wiped on every update. Carried
      // unconditionally, like the update verdict below, because the install has
      // no opinion about which categories a mod belongs to.
      if (!replaced_categories.empty())
        meta.set("General", "category", replaced_categories);

      if (existing_priority >= 0)
        meta.set_priority(existing_priority);

      // Separator membership is not derived from anything the install knows, so
      // it is carried the same way. Empty means the mod was not in a separator.
      if (meta.separator_id().empty() && !replaced_separator.empty())
        meta.set_separator_id(replaced_separator);

      // The update verdict, carried for the same reason and with the same
      // "only when the new meta does not already hold one" rule. The verdict
      // belongs to the MOD, not to the file that was installed: the sweep
      // compared the newest file at the source against what was installed,
      // and only the caller that knows the new version can now say whether
      // that still holds. Carrying it forward is what gives
      // record_installed_version something to reconcile - without these keys
      // its first gate reads an ABSENT flag and the row keeps wearing the
      // update symbol for a mod that is now current. Re-stamped by the sweep
      // and by "Ignore this update" like any other version-shaped key, so
      // they follow the newest answer rather than freezing here.
      // nexus_update is Nexus's key, update_available the page-scrape
      // sources'; each source only ever writes its own, so carrying both is
      // a no-op for the other source.
      if (!replaced_nexus_update.empty())
        meta.set("General", "nexus_update", replaced_nexus_update);
      const std::string replaced_available =
          replaced_meta.get("General", "update_available");
      if (!replaced_available.empty())
        meta.set("General", "update_available", replaced_available);
      if (meta.get("General", "newestversion").empty() && !replaced_newest.empty())
        meta.set("General", "newestversion", replaced_newest);
      if (!replaced_ignored.empty() &&
          engine::Source::Nexus::UpdateCheck::read_ignored_version(meta).empty())
        meta.set("General", "ignoredVersion", replaced_ignored);

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

      // modl:// is a URI handler, not a source - the [Modl] section
      // is no longer written. Source attribution for modl-derived downloads
      // comes from the direct URL's host (see Router::derive_source_from_direct_url):
      // a mod.pub direct URL -> [ModPub] below; any other host names no source,
      // so the mod is recorded as [GameModManager] source_type="manual" with no
      // provider section. Legacy mods with a [Modl] section still read it via
      // source_page_url() for backward compatibility; the section is never
      // overwritten.

      // ModPub is metadata-only - downloads route through modl://. Persist
      // the mod id, the canonical mod.pub page URL (carries game-slug + the
      // slug-suffix, both unrecoverable from the bare id), the game-slug
      // itself (so the panel's Visit fallback can build a correct URL
      // before the first Refresh), and the display name. source_type stays
      // "modpub" (single-source rule fqf5: modpub is its own source, never
      // folded into nexus).
      if (mod.download_source_type == "modpub") {
        if (!mod.download_source_id.empty())
          meta.set("ModPub", "mod_id", mod.download_source_id);
        if (!mod.download_page_url.empty())
          meta.set("ModPub", "page_url", mod.download_page_url);
        const std::string slug =
            engine::Source::ModPub::Provider::extract_game_slug(mod.download_page_url);
        if (!slug.empty())
          meta.set("ModPub", "game_slug", slug);
        if (!mod.name.empty())
          meta.set("ModPub", "display_name", mod.name);
      }

      if (!meta.save_file(meta_path)) {
        Logger::instance().warn("InstallStage: failed to write meta.ini for " +
                                folder_name);
      }
    }

    mod.id    = folder_name;
    mod.state = ModState::Installed;
    Logger::instance().debug("InstallStage: installed " + folder_name);
    return true;
  }

}  // namespace Source

}  // namespace engine
