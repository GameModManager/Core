#include "ui/main_window/mod_scan_worker.h"

#include "engine/core/log/logger.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/mod/meta/mod_meta.h"

#include <QMetaObject>
#include <QThread>

#include <algorithm>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace ui {

namespace {

  // Where loose plugin files (vanilla ESMs, stray unmanaged esp/esm/esl) and
  // IUnmanagedMods-declared folders live on disk - i.e. the game's actual
  // data dir, NOT a mods source. Distinct from resolve_game_mods_dir, which
  // resolves the SCAN SOURCE and intentionally returns empty for games that
  // only declare mods_subpath (deploy-only): walking the install root as a
  // scan source would synthesize vanilla content (Data/, SKSE, Scripts,
  // Meshes, Source, ...) as ScannedMod rows, which is exactly the MO2
  // behavior the bug ticket is fixing. Per-file synthesis (unmanaged
  // plugins, vanilla ESMs) does NOT walk the folder - it just stats one
  // file at a time - so it is safe to derive native_dir from
  // game_dir/mods_subpath here.
  //
  // When the caller supplied an override or the plugin declared an explicit
  // "game_mods_dir" hook (Isaac on macOS), that path IS the data dir -
  // loose plugins and the game's vanilla ESMs live there, not in
  // game_dir/mods_subpath. The override / plugin hook always wins so the
  // stray-plugin synthesis keeps matching the plugin's actual on-disk
  // reality.
  std::filesystem::path native_dir_for(const std::string& game_id,
                                       const std::filesystem::path& game_dir,
                                       const engine::GameKnowledge& knowledge,
                                       const std::filesystem::path& override_dir) {
    if (!override_dir.empty())
      return override_dir;
    // Plugin hook (absolute OR relative -> game_dir). Anchors Isaac on
    // Linux/Windows "mods" to game_dir/mods, matching the scan/deploy target.
    const auto plugin_declared =
        engine::resolve_plugin_game_mods_dir(game_id, game_dir, knowledge);
    if (!plugin_declared.empty())
      return plugin_declared;
    const std::string subpath = knowledge.get(game_id, "mods_subpath", "");
    if (subpath.empty())
      return game_dir;
    return game_dir / subpath;
  }

}  // namespace

ModScanWorker::ModScanWorker(QObject* parent) : QObject(parent) {}

void ModScanWorker::run(ModScanRequest request, quint64 generation) {
  ModScanResult result;
  auto& scanned = result.scanned;

  const auto& knowledge = request.knowledge;
  const auto& game_id   = request.game_id;

  // Mod sources, MO2-style:
  //   - In instance mode: the instance's own mods dir (<instance>/mods) is
  //     the ONLY legitimate scan source for mod folders.
  //   - When a plugin declares a real external "game_mods_dir" hook (Isaac
  //     on macOS) or the user set the instance.toml "game_mods_dir"
  //     override to a genuinely external folder, that folder is also a
  //     scan source - it is, by construction, a mods-only staging folder.
  //   - The game's install root / Data/ is NEVER a scan source. Its
  //     vanilla content is read-only. Loose plugin files (esp/esm/esl)
  //     there are picked up below the per-file synthesis blocks, never as
  //     a folder-level scan.
  // The previous code walked game_dir unconditionally and then either
  // merged or replaced the result via the mod_scan_subpath/mods_subpath
  // fallback chain. That whole branch is gone: a folder-level scan
  // against the game install is exactly the regression the ticket is
  // fixing.
  const bool explicit_game_mods_dir =
      !request.game_mods_dir.empty() ||
      !engine::plugin_game_mods_dir(knowledge, game_id).empty();

  // 1. Primary scan: instance mods dir when one exists (always - this is
  //    the only legitimate mod source in instance mode). Portable mode
  //    (no instance_root) skips the scan; the caller can fall back to
  //    its own scan via game_mods_dir below if it wishes.
  if (!request.instance_root.empty() && !request.mods_dir.empty()) {
    auto inst_scanned = engine::ModScanner::scan_dir(
        knowledge, game_id, request.mods_dir, std::vector<std::filesystem::path>{});
    engine::Logger::instance().debug("ModScanWorker: instance mods dir scan found " +
                                     std::to_string(inst_scanned.size()) +
                                     " mod(s) at " + request.mods_dir.string());
    scanned = std::move(inst_scanned);
  }

  // 2. External game-mods dir (genuinely external, set via the plugin
  //    "game_mods_dir" hook or the instance.toml override). Folder-level
  //    scan ONLY when this is the legitimately-external case - never
  //    when the resolved dir would be game_dir or game_dir/mods_subpath
  //    (those are vanilla game content, not a mods source). Merging in
  //    is safe because the folder is by construction a mods-only
  //    staging dir (no vanilla content), and dedup-by-folder-name keeps
  //    a mod already in the instance mods dir from appearing twice.
  if (explicit_game_mods_dir) {
    // Resolve the full chain (override -> plugin hook [absolute or
    // relative-to-game_dir]) so a relative plugin declaration
    // (Isaac on Linux/Windows "mods") lands at game_dir/mods. The
    // override path is passed as-is so callers that already suppress
    // resolution (current_game_mods_dir in main_window) keep working.
    const std::filesystem::path external = engine::resolve_game_mods_dir(
        game_id, request.game_dir, knowledge, request.game_mods_dir.string());
    if (!external.empty() && external != request.mods_dir) {
      auto ext_scanned = engine::ModScanner::scan_dir(
          knowledge, game_id, external, std::vector<std::filesystem::path>{});
      const auto kept = scanned.size();
      std::unordered_set<std::string> existing;
      for (const auto& m : scanned)
        existing.insert(m.folder_name);
      int upgraded = 0;
      for (auto& m : ext_scanned) {
        auto it = existing.find(m.folder_name);
        if (it == existing.end()) {
          // New mod from external dir - set its content_dir to the external
          // path (the actual game files live there).
          m.content_dir = external / m.folder_name;
          existing.insert(m.folder_name);
          scanned.push_back(std::move(m));
        } else {
          // Duplicate: instance version exists. When the instance version
          // is metadata-poor (no_metadata=true) and the external version
          // has real metadata, upgrade the instance entry with the
          // external metadata. This handles Isaac: instance/mods/ has a
          // stub folder with only meta.ini, while game_dir/mods/ has the
          // real metadata.xml, content files, version, etc.
          for (auto& inst : scanned) {
            if (inst.folder_name != m.folder_name)
              continue;
            if (inst.no_metadata && !m.no_metadata) {
              // Upgrade metadata from external scan
              inst.display_name     = m.display_name;
              inst.raw_name         = m.raw_name;
              inst.version          = m.version;
              inst.no_metadata      = false;
              inst.invalid_data     = m.invalid_data;
              inst.has_hidden_files = m.has_hidden_files;
              inst.is_empty         = m.is_empty;
              inst.category_ids     = m.category_ids;
              inst.workshop_id      = m.workshop_id;
              ++upgraded;
            }
            // Content lives in the external dir regardless of metadata.
            // Path resolution (file open, mod info) needs this.
            inst.content_dir = external / inst.folder_name;
            // Mirror/backup marker (Workspace-0pi5): the external source's
            // [Mirror] section marks the merged row mirrored while the
            // source is present (badge, no behavior change). The instance
            // backup carries the same section, so a rescan after a source
            // deletion still flags the row source-missing via its own meta.
            if (m.is_mirrored) {
              inst.is_mirrored           = true;
              inst.mirror_source_path    = m.mirror_source_path;
              inst.mirror_source_missing = false;
            }
            break;
          }
        }
      }
      engine::Logger::instance().debug(
          "ModScanWorker: merged external game-mods-dir scan, " +
          std::to_string(scanned.size() - kept) + " added, " +
          std::to_string(upgraded) + " upgraded, " + std::to_string(scanned.size()) +
          " total");
    }
  }

  // Detect game-native plugins (e.g. vanilla ESMs) and synthesize
  // unmanaged rows for stray plugins dropped straight into the game's
  // Data dir (MO2's UnmanagedMods behavior) so the mod<->plugin
  // selection highlight round-trips for files with no owning mod. A
  // file a mod folder already covers is skipped here - the ownership
  // join (GamePlugin::owner_mod) decides which row highlights for
  // shadowed strays instead. Game-less instances skip the whole block:
  // there is no game_dir to look at (Workspace-wk8).
  if (!request.game_dir.empty()) {
    auto native_plugins = engine::native_plugins_csv(knowledge, game_id);
    if (!native_plugins.empty()) {
      const std::filesystem::path native_dir =
          native_dir_for(game_id, request.game_dir, knowledge, request.game_mods_dir);

      std::unordered_set<std::string> existing;
      for (const auto& m : scanned)
        existing.insert(m.folder_name);

      std::unordered_set<std::string> declared_native;
      std::istringstream ss(native_plugins);
      std::string plugin;
      while (std::getline(ss, plugin, ',')) {
        auto start = plugin.find_first_not_of(" \t");
        auto end   = plugin.find_last_not_of(" \t");
        if (start == std::string::npos)
          continue;
        plugin = plugin.substr(start, end - start + 1);
        declared_native.insert(plugin);
        if (existing.count(plugin))
          continue;

        auto plugin_path = native_dir / plugin;
        if (!std::filesystem::exists(plugin_path))
          continue;

        engine::ScannedMod native_mod;
        native_mod.folder_name    = plugin;
        native_mod.display_name   = plugin;
        native_mod.raw_name       = plugin;
        native_mod.is_game_native = true;
        native_mod.enabled        = true;
        scanned.push_back(std::move(native_mod));
      }

      std::error_code scan_ec;
      if (std::filesystem::is_directory(native_dir, scan_ec)) {
        // Deployed plugins (direct-symlink mode) live in the game's
        // Data dir as symlinks back into the instance mods folder. The
        // deploy ledger is the source of truth for what we deployed: a
        // plugin whose path is a ledger target must NOT be synthesized
        // as an unmanaged row. Compare via weakly_canonical on both
        // sides so a symlinked/differently-spelled game dir still
        // matches, and only canonicalize plugin targets (the stray
        // scan only cares about plugins).
        std::unordered_set<std::filesystem::path> deployed_plugins;
        if (!request.ledger_file.empty()) {
          for (const auto& [target, source] :
               engine::load_deploy_ledger(request.ledger_file)) {
            (void)source;
            if (!engine::is_plugin_file(target))
              continue;
            std::error_code cec;
            auto canon = std::filesystem::weakly_canonical(target, cec);
            if (!cec)
              deployed_plugins.insert(std::move(canon));
          }
        }

        for (const auto& entry :
             std::filesystem::directory_iterator(native_dir, scan_ec)) {
          // Deployed .esp files are always symlinks (only executables
          // are copied as real files, and .esp is never executable):
          // skip symlinks outright. A real file the user dropped in
          // still flows through to the unmanaged synthesis below.
          std::error_code sec;
          if (entry.is_symlink(sec))
            continue;
          if (!entry.is_regular_file(scan_ec))
            continue;
          if (!engine::is_plugin_file(entry.path()))
            continue;
          const std::string file = entry.path().filename().string();
          if (declared_native.count(file) || existing.count(file))
            continue;
          if (!deployed_plugins.empty()) {
            std::error_code cec;
            auto canon = std::filesystem::weakly_canonical(entry.path(), cec);
            if (!cec && deployed_plugins.count(canon))
              continue;
          }
          engine::ScannedMod stray_mod;
          stray_mod.folder_name    = file;
          stray_mod.display_name   = file;
          stray_mod.raw_name       = file;
          stray_mod.is_game_native = true;
          stray_mod.enabled        = true;
          scanned.push_back(std::move(stray_mod));
        }
      }
    }
  }

  // Registered unmanaged_mods feature (MO2 IUnmanagedMods): mods the game
  // manages itself (DLC/CC folders, plugin-less game dirs) that a plugin
  // declares and that must appear in the list as unmanaged rows. A file or
  // folder with the declared internal name inside the game's mods dir
  // becomes a row; anything a scan row already covers is skipped. Also
  // game-dir-dependent - skipped for game-less instances (Workspace-wk8).
  if (!request.game_dir.empty()) {
    auto unmanaged = engine::unmanaged_mods_for(game_id);
    if (!unmanaged.empty()) {
      const std::filesystem::path native_dir =
          native_dir_for(game_id, request.game_dir, knowledge, request.game_mods_dir);

      std::unordered_set<std::string> existing;
      for (const auto& m : scanned)
        existing.insert(m.folder_name);

      for (const auto& name : unmanaged) {
        if (name.empty() || existing.count(name))
          continue;
        std::error_code ec;
        if (!std::filesystem::exists(native_dir / name, ec))
          continue;
        engine::ScannedMod unmanaged_mod;
        unmanaged_mod.folder_name    = name;
        unmanaged_mod.display_name   = name;
        unmanaged_mod.raw_name       = name;
        unmanaged_mod.is_game_native = true;
        unmanaged_mod.enabled        = true;
        scanned.push_back(std::move(unmanaged_mod));
      }
    }
  }

  // One-time migration of manager sidecars ({instance_root}/meta/*.ini)
  // into the MO2-compatible in-folder location (mods/{folder}/meta.ini),
  // plus in-place enrichment of raw MO2 meta.ini files. Runs here so the
  // load path does no directory walking on the main thread. Idempotent:
  // folders without a sidecar and already-enriched folders are skipped,
  // so re-runs are cheap.
  if (std::filesystem::exists(request.mods_dir)) {
    // Steam appid for this game (needed for Workshop mods)
    auto steam_appid_str  = knowledge.get(game_id, "steam_appid", "0");
    const bool have_appid = !steam_appid_str.empty() && steam_appid_str != "0";

    // Legacy sidecars live at {instance_root}/meta (never overridden -
    // Instance has no path override for them). Empty in portable mode.
    const std::filesystem::path legacy_dir = request.instance_root.empty()
                                                 ? std::filesystem::path{}
                                                 : request.instance_root / "meta";

    // NOTE: meta.bak/ is a rescue copy and is never wiped here - it must
    // survive at least one release (Workspace-pmrh M1). The orphan sweep
    // below never overwrites an existing rescue copy either.

    // Merge a legacy sidecar into the in-folder base: the sidecar was the
    // source of truth for manager state, the folder may hold newer
    // game-written data (MO2-touched installs).
    auto merge_sidecar = [](engine::ModMeta& base, const engine::ModMeta& side) {
      for (const auto& key : side.keys("GameModManager"))
        base.set("GameModManager", key, side.get("GameModManager", key));
      for (const char* sec :
           {"Nexusmods", "LoversLab", "ModPub", "SteamWorkshop", "Modl"}) {
        for (const auto& key : side.keys(sec))
          base.set(sec, key, side.get(sec, key));
      }
      // The category CSV is manager state (Categories tab writes it);
      // install stamps ride along too. The rest of [General] is
      // game-owned and stays as the folder has it.
      for (const char* key : {"category", "installed", "installationfile"}) {
        const auto v = side.get("General", key);
        if (!v.empty())
          base.set("General", key, v);
      }
    };

    // Move a legacy sidecar into folder_dir/meta.ini: merge when the
    // folder already has one (the sidecar wins manager keys, the folder
    // wins game keys), plain move otherwise. Stamps steam_appid when
    // known. Returns true when the sidecar was consumed. Used for
    // instance folders below, and for external game-dir folders + the
    // meta.bak/ recovery in the orphan sweep (Workspace-7tjz).
    auto move_sidecar_into = [&](const std::filesystem::path& sidecar,
                                 const std::filesystem::path& folder_dir,
                                 const std::string& folder_name) {
      const auto in_folder = folder_dir / "meta.ini";
      std::error_code mec2;
      if (std::filesystem::exists(in_folder, mec2)) {
        auto merged = engine::ModMeta::load_file(in_folder);
        merge_sidecar(merged, engine::ModMeta::load_file(sidecar));
        if (have_appid)
          merged.set("GameModManager", "steam_appid", steam_appid_str);
        if (merged.save_file(in_folder)) {
          std::filesystem::remove(sidecar, mec2);
          if (mec2)
            engine::Logger::instance().warn("Failed to remove migrated sidecar: " +
                                            sidecar.string());
          return true;
        }
        engine::Logger::instance().warn("Failed to save migrated meta: " + folder_name);
        return false;
      }
      // Sidecar only (XML-metadata games, separators): move it into
      // the folder (rename; copy+delete across filesystems).
      std::error_code mv_ec;
      std::filesystem::rename(sidecar, in_folder, mv_ec);
      if (mv_ec) {
        std::filesystem::copy_file(sidecar, in_folder, mv_ec);
        if (!mv_ec)
          std::filesystem::remove(sidecar, mv_ec);
      }
      if (mv_ec) {
        engine::Logger::instance().warn("Failed to migrate sidecar: " + folder_name);
        return false;
      }
      auto migrated = engine::ModMeta::load_file(in_folder);
      if (have_appid)
        migrated.set("GameModManager", "steam_appid", steam_appid_str);
      (void)migrated.save_file(in_folder);
      engine::Logger::instance().debug("Migrated sidecar meta: " + folder_name +
                                       "/meta.ini");
      return true;
    };

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(request.mods_dir)) {
      if (!entry.is_directory())
        continue;
      auto folder_name     = entry.path().filename().string();
      const auto in_folder = entry.path() / "meta.ini";

      const bool has_sidecar =
          !legacy_dir.empty() &&
          std::filesystem::exists(legacy_dir / (folder_name + ".ini"), ec);

      if (has_sidecar) {
        const auto sidecar = legacy_dir / (folder_name + ".ini");
        (void)move_sidecar_into(sidecar, entry.path(), folder_name);
        continue;
      }

      // No sidecar: enrich a raw MO2 meta.ini in place (first scan of an
      // MO2-imported instance). Already-enriched folders (a
      // [GameModManager] section exists) are skipped.
      if (!engine::ModMeta::has_mo2_meta(entry.path()))
        continue;
      if (engine::ModMeta::load_file(in_folder).has_section("GameModManager"))
        continue;

      // Import
      auto meta = engine::ModMeta::import_mo2(entry.path(), folder_name);
      if (!meta.has_section("General") && !meta.has_section("GameModManager"))
        continue;

      // Fill in the steam_appid from game knowledge
      if (have_appid) {
        meta.set("GameModManager", "steam_appid", steam_appid_str);
      }

      if (meta.save_file(in_folder)) {
        engine::Logger::instance().debug("Imported MO2 meta: " + folder_name +
                                         "/meta.ini");
      } else {
        engine::Logger::instance().warn("Failed to save imported meta: " + folder_name);
      }
    }

    // The external game-mods dir (Isaac's game_dir/mods/ via the plugin
    // hook or the instance.toml override) - the same resolution the
    // external scan above uses. Needed below so game-dir-only mods are
    // not mistaken for orphans (Workspace-7tjz).
    const std::filesystem::path external_mods_dir = engine::resolve_game_mods_dir(
        game_id, request.game_dir, knowledge, request.game_mods_dir.string());
    const auto bak_dir = request.instance_root / "meta.bak";

    // Orphan sidecars (no matching mod folder - renamed/deleted mods) move
    // to meta.bak/ for one release instead of being deleted outright.
    // Before orphaning, check the external game-mods dir too: Steam
    // Workshop mods (e.g. Isaac) live ONLY in game_dir/mods/, never in
    // the instance mods dir - sweeping their sidecars to meta.bak/ would
    // lose all manager state (source_type, source_id, categories,
    // workshop_id). A folder in EITHER dir is migrated next to the mod
    // it describes (instance folder preferred); an instance stub is never
    // created for a game-dir mod - mkdir-ing mods/{folder}/ would promote
    // the row into a scanned instance mod (Workspace-pmrh H1).
    if (!legacy_dir.empty() && std::filesystem::exists(legacy_dir, ec)) {
      int orphans = 0;
      int rescued = 0;
      for (const auto& sentry : std::filesystem::directory_iterator(legacy_dir, ec)) {
        std::error_code sec;
        if (!sentry.is_regular_file(sec) || sentry.path().extension() != ".ini")
          continue;
        const auto folder = sentry.path().stem().string();
        std::filesystem::path target;
        if (std::filesystem::is_directory(request.mods_dir / folder, sec))
          target = request.mods_dir / folder;
        else if (!external_mods_dir.empty() && external_mods_dir != request.mods_dir &&
                 std::filesystem::is_directory(external_mods_dir / folder, sec))
          target = external_mods_dir / folder;
        if (!target.empty()) {
          if (move_sidecar_into(sentry.path(), target, folder))
            ++rescued;
          continue;
        }
        // Never overwrite a previous rescue copy of the same sidecar - the
        // older copy stays until the user restores or deletes it by hand.
        // The leftover sidecar is retried (and skipped again) next scan.
        std::error_code bak_ec;
        if (std::filesystem::exists(bak_dir / sentry.path().filename(), bak_ec))
          continue;
        std::error_code mec;
        std::filesystem::create_directories(bak_dir, mec);
        std::filesystem::rename(sentry.path(), bak_dir / sentry.path().filename(), mec);
        if (mec) {
          std::filesystem::copy_file(sentry.path(), bak_dir / sentry.path().filename(),
                                     mec);
          if (!mec)
            std::filesystem::remove(sentry.path(), mec);
        }
        if (!mec)
          ++orphans;
      }
      if (orphans > 0)
        engine::Logger::instance().debug("Moved " + std::to_string(orphans) +
                                         " orphan sidecars to meta.bak/");
      if (rescued > 0)
        engine::Logger::instance().debug(
            "Migrated " + std::to_string(rescued) +
            " game-dir sidecars in-folder (Workspace-7tjz)");
    }

    // Recovery (Workspace-7tjz): a previous release's orphan sweep may
    // already have buried live mods' sidecars in meta.bak/ (Isaac
    // game-dir mods swept before the external-dir check existed). On
    // every scan, restore any rescue copy whose folder exists again -
    // instance folder first, then the external game-mods dir. An
    // existing in-folder meta.ini is newer truth and never overwritten;
    // that rescue copy stays in meta.bak/ for a manual restore instead.
    if (!legacy_dir.empty() && std::filesystem::is_directory(bak_dir, ec)) {
      int restored = 0;
      for (const auto& bentry : std::filesystem::directory_iterator(bak_dir, ec)) {
        std::error_code sec;
        if (!bentry.is_regular_file(sec) || bentry.path().extension() != ".ini")
          continue;
        const auto folder = bentry.path().stem().string();
        std::filesystem::path target;
        if (std::filesystem::is_directory(request.mods_dir / folder, sec))
          target = request.mods_dir / folder;
        else if (!external_mods_dir.empty() && external_mods_dir != request.mods_dir &&
                 std::filesystem::is_directory(external_mods_dir / folder, sec))
          target = external_mods_dir / folder;
        if (target.empty())
          continue;
        std::error_code tex;
        if (std::filesystem::exists(target / "meta.ini", tex))
          continue;
        std::error_code mv_ec;
        std::filesystem::rename(bentry.path(), target / "meta.ini", mv_ec);
        if (mv_ec) {
          std::filesystem::copy_file(bentry.path(), target / "meta.ini", mv_ec);
          if (!mv_ec)
            std::filesystem::remove(bentry.path(), mv_ec);
        }
        if (!mv_ec) {
          auto revived = engine::ModMeta::load_file(target / "meta.ini");
          if (have_appid)
            revived.set("GameModManager", "steam_appid", steam_appid_str);
          (void)revived.save_file(target / "meta.ini");
          ++restored;
        }
      }
      if (restored > 0)
        engine::Logger::instance().debug("Restored " + std::to_string(restored) +
                                         " sidecars from meta.bak/ (Workspace-7tjz)");
    }

    // Prune ghost stubs (Workspace-5jk3): when Steam unsubscribes from a
    // mod it deletes the files it installed but leaves the GMM-written
    // meta.ini behind, so the folder rescans forever as is_empty. The
    // helper deletes ONLY tracked-but-now-empty instance folders (never
    // separators, Overwrite/MERGED, game-native rows, or anything in the
    // Steam-owned external dir). Runs on the worker thread with the merged
    // scan flags: a stub whose external content is still healthy merged
    // is_empty=false and survives.
    const auto pruned = engine::ModScanner::prune_orphaned_empty_mods(
        scanned, request.mods_dir, request.instance_root);
    if (!pruned.empty()) {
      const std::unordered_set<std::string> gone(pruned.begin(), pruned.end());
      scanned.erase(std::remove_if(scanned.begin(), scanned.end(),
                                   [&gone](const engine::ScannedMod& m) {
                                     return gone.count(m.folder_name) > 0;
                                   }),
                    scanned.end());
      engine::Logger::instance().debug("Pruned " + std::to_string(pruned.size()) +
                                       " orphaned empty mod(s) (Workspace-5jk3)");
    }
  }

  emit finished(std::move(result), generation);
}

ModScanThread::ModScanThread(QObject* parent) : QObject(parent) {
  qRegisterMetaType<ui::ModScanResult>();
  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("gmm-mod-scan"));
  worker_ = new ModScanWorker(nullptr);
  worker_->moveToThread(thread_);
  connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
  thread_->start();
}

ModScanThread::~ModScanThread() {
  thread_->quit();
  thread_->wait();
}

void ModScanThread::start(ModScanRequest request, quint64 generation) {
  ModScanWorker* worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, req = std::move(request), gen = generation]() mutable {
        worker->run(std::move(req), gen);
      },
      Qt::QueuedConnection);
}

}  // namespace ui
