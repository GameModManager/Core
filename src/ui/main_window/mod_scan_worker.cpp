#include "ui/main_window/mod_scan_worker.h"

#include "engine/core/log/logger.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_features/game_feature_registry.h"
#include "engine/mod/meta/mod_meta.h"

#include <QMetaObject>
#include <QThread>

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
std::filesystem::path
native_dir_for(const std::string &game_id,
               const std::filesystem::path &game_dir,
               const engine::GameKnowledge &knowledge,
               const std::filesystem::path &override_dir) {
  if (!override_dir.empty())
    return override_dir;
  const std::string plugin_declared =
      engine::plugin_game_mods_dir(knowledge, game_id);
  if (!plugin_declared.empty())
    return std::filesystem::path(plugin_declared);
  const std::string subpath = knowledge.get(game_id, "mods_subpath", "");
  if (subpath.empty())
    return game_dir;
  return game_dir / subpath;
}

} // namespace

ModScanWorker::ModScanWorker(QObject *parent) : QObject(parent) {}

void ModScanWorker::run(ModScanRequest request, quint64 generation) {
  ModScanResult result;
  auto &scanned = result.scanned;

  const auto &knowledge = request.knowledge;
  const auto &game_id = request.game_id;

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
    auto inst_scanned =
        engine::ModScanner::scan_dir(knowledge, game_id, request.mods_dir,
                                     std::vector<std::filesystem::path>{});
    engine::Logger::instance().debug(
        "ModScanWorker: instance mods dir scan found " +
        std::to_string(inst_scanned.size()) + " mod(s) at " +
        request.mods_dir.string());
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
    // Use the caller-provided override path verbatim: callers
    // (current_game_mods_dir in main_window) already suppress the
    // resolution when game_mods_dir == mods_dir_path().
    const auto &external =
        !request.game_mods_dir.empty()
            ? request.game_mods_dir
            : std::filesystem::path(
                  engine::plugin_game_mods_dir(knowledge, game_id));
    if (!external.empty() && external != request.mods_dir) {
      auto ext_scanned = engine::ModScanner::scan_dir(
          knowledge, game_id, external, std::vector<std::filesystem::path>{});
      const auto kept = scanned.size();
      std::unordered_set<std::string> existing;
      for (const auto &m : scanned)
        existing.insert(m.folder_name);
      for (auto &m : ext_scanned)
        if (existing.insert(m.folder_name).second)
          scanned.push_back(std::move(m));
      engine::Logger::instance().debug(
          "ModScanWorker: merged external game-mods-dir scan, " +
          std::to_string(scanned.size() - kept) + " added, " +
          std::to_string(scanned.size()) + " total");
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
      const std::filesystem::path native_dir = native_dir_for(
          game_id, request.game_dir, knowledge, request.game_mods_dir);

      std::unordered_set<std::string> existing;
      for (const auto &m : scanned)
        existing.insert(m.folder_name);

      std::unordered_set<std::string> declared_native;
      std::istringstream ss(native_plugins);
      std::string plugin;
      while (std::getline(ss, plugin, ',')) {
        auto start = plugin.find_first_not_of(" \t");
        auto end = plugin.find_last_not_of(" \t");
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
        native_mod.folder_name = plugin;
        native_mod.display_name = plugin;
        native_mod.raw_name = plugin;
        native_mod.is_game_native = true;
        native_mod.enabled = true;
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
          for (const auto &[target, source] :
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

        for (const auto &entry :
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
          stray_mod.folder_name = file;
          stray_mod.display_name = file;
          stray_mod.raw_name = file;
          stray_mod.is_game_native = true;
          stray_mod.enabled = true;
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
      const std::filesystem::path native_dir = native_dir_for(
          game_id, request.game_dir, knowledge, request.game_mods_dir);

      std::unordered_set<std::string> existing;
      for (const auto &m : scanned)
        existing.insert(m.folder_name);

      for (const auto &name : unmanaged) {
        if (name.empty() || existing.count(name))
          continue;
        std::error_code ec;
        if (!std::filesystem::exists(native_dir / name, ec))
          continue;
        engine::ScannedMod unmanaged_mod;
        unmanaged_mod.folder_name = name;
        unmanaged_mod.display_name = name;
        unmanaged_mod.raw_name = name;
        unmanaged_mod.is_game_native = true;
        unmanaged_mod.enabled = true;
        scanned.push_back(std::move(unmanaged_mod));
      }
    }
  }

  // One-time import of MO2 meta.ini sidecars into the manager's meta dir
  // (migrate_mo2_meta). Runs here so the load path does no directory walking
  // on the main thread. Idempotent: folders whose meta is already imported
  // are skipped, so re-runs are cheap.
  if (!request.meta_dir.empty() && std::filesystem::exists(request.mods_dir)) {
    // Steam appid for this game (needed for Workshop mods)
    auto steam_appid_str = knowledge.get(game_id, "steam_appid", "0");

    for (const auto &entry :
         std::filesystem::directory_iterator(request.mods_dir)) {
      if (!entry.is_directory())
        continue;
      auto folder_name = entry.path().filename().string();

      // Skip if meta already imported
      if (engine::ModMeta::exists(request.meta_dir, folder_name))
        continue;

      // Check if MO2 meta.ini exists in this mod's folder
      if (!engine::ModMeta::has_mo2_meta(entry.path()))
        continue;

      // Import
      auto meta = engine::ModMeta::import_mo2(entry.path(), folder_name);
      if (!meta.has_section("General") && !meta.has_section("GameModManager"))
        continue;

      // Fill in the steam_appid from game knowledge
      if (!steam_appid_str.empty() && steam_appid_str != "0") {
        meta.set("GameModManager", "steam_appid", steam_appid_str);
      }

      if (meta.save(request.meta_dir, folder_name)) {
        engine::Logger::instance().debug("Imported MO2 meta: " + folder_name +
                                         "/meta.ini");
      } else {
        engine::Logger::instance().warn("Failed to save imported meta: " +
                                        folder_name);
      }
    }
  }

  emit finished(std::move(result), generation);
}

ModScanThread::ModScanThread(QObject *parent) : QObject(parent) {
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
  ModScanWorker *worker = worker_;
  QMetaObject::invokeMethod(
      worker,
      [worker, req = std::move(request), gen = generation]() mutable {
        worker->run(std::move(req), gen);
      },
      Qt::QueuedConnection);
}

} // namespace ui
