#include "ui/main_window/mod_scan_worker.h"

#include "engine/core/log/logger.h"
#include "engine/deploy/deploy_utils.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/registry/game_features/game_feature_registry.h"

#include <QMetaObject>
#include <QThread>

#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace ui {

ModScanWorker::ModScanWorker(QObject* parent) : QObject(parent) {}

void ModScanWorker::run(ModScanRequest request, quint64 generation) {
    ModScanResult result;
    auto& scanned = result.scanned;

    const auto& knowledge = request.knowledge;
    const auto& game_id = request.game_id;

    // Ignore symlink targets: in instance mode the instance mods dir lives
    // under the instance root, which ModScanner::scan must not follow back
    // into duplicate entries.
    const std::vector<std::filesystem::path> ignore_symlink_targets =
        request.instance_root.empty()
            ? std::vector<std::filesystem::path>{}
            : std::vector<std::filesystem::path>{request.instance_root};

    // Scan game's native mods directory. Skipped for game-less instances
    // (Workspace-wk8): an empty path would resolve against the CWD. When the
    // resolved dir is an explicit external mods folder this scan finds the
    // real deployed mods; otherwise (plain game_dir/mods_subpath) the
    // instance-mode block below replaces it with the instance mods dir.
    if (!request.game_dir.empty()) {
        // Workspace-93m: scan() resolves through resolve_game_mods_dir —
        // instance override > plugin "game_mods_dir" hook > game_dir/mods_subpath.
        // A set game_mods_dir IS the mods folder; nothing is appended.
        scanned = engine::ModScanner::scan(knowledge, game_id, request.game_dir,
                                           ignore_symlink_targets,
                                           request.game_mods_dir);
        engine::Logger::instance().debug("ModScanWorker: game-dir scan found " +
                                 std::to_string(scanned.size()) + " mod(s)");
    }

    // Instance mode: when the game's mods dir is an explicit external folder
    // (instance.toml override or plugin "game_mods_dir" hook, Workspace-6up),
    // the game-dir scan above already found the real deployed mods - keep it
    // and merge in what GMM stores in the instance mods dir (downloaded but
    // not yet deployed), deduped by folder name (Workspace-8j8). When the
    // game dir resolved to plain game_dir/mods_subpath instead, its folders
    // are vanilla game content, not mods (e.g. Skyrim's Data/Scripts,
    // Data/Video) - MO2 lists only <instance>/mods, so replace.
    const bool explicit_game_mods_dir =
        !request.game_mods_dir.empty() ||
        !engine::plugin_game_mods_dir(knowledge, game_id).empty();
    if (!request.instance_root.empty()) {
        // Game-native mods dir: instance.toml override > plugin-declared
        // "game_mods_dir" hook > game_dir/mods_subpath (Workspace-otx).
        auto game_mods_dir = engine::resolve_game_mods_dir(
            game_id, request.game_dir, knowledge,
            request.game_mods_dir.string());
        // Only scan separately if they're different directories
        std::error_code ec_canon;
        auto inst_canon = std::filesystem::weakly_canonical(request.mods_dir, ec_canon);
        auto game_canon = std::filesystem::weakly_canonical(game_mods_dir, ec_canon);
        engine::Logger::instance().debug(
            "ModScanWorker: instance mode (instance_root=" + request.instance_root.string() +
            ") inst_mods_dir=" + inst_canon.string() +
            " game_mods_dir=" + game_canon.string() +
            " same=" + std::to_string(inst_canon == game_canon ? 1 : 0));
        if (inst_canon != game_canon) {
            auto inst_scanned = engine::ModScanner::scan_dir(
                knowledge, game_id, request.mods_dir,
                std::vector<std::filesystem::path>{});
            if (explicit_game_mods_dir) {
                const auto kept = scanned.size();
                std::unordered_set<std::string> existing;
                for (const auto& m : scanned)
                    existing.insert(m.folder_name);
                for (auto& m : inst_scanned)
                    if (existing.insert(m.folder_name).second)
                        scanned.push_back(std::move(m));
                engine::Logger::instance().debug(
                    "ModScanWorker: merged instance mods dir into game-mods-dir scan, " +
                    std::to_string(scanned.size() - kept) + " added, " +
                    std::to_string(scanned.size()) + " total");
            } else {
                scanned = std::move(inst_scanned);
                engine::Logger::instance().debug("ModScanWorker: game-dir scan REPLACED by instance mods dir, " +
                                         std::to_string(scanned.size()) + " mod(s)");
            }
        } else {
            engine::Logger::instance().debug("ModScanWorker: instance mods dir == game mods dir, keeping game-dir scan");
        }
    }

    // Detect game-native plugins (e.g. vanilla ESMs) from the game's mods
    // directory, and synthesize unmanaged rows for stray plugins dropped
    // straight into the game's Data dir (MO2's UnmanagedMods behavior) so the
    // mod<->plugin selection highlight round-trips for files with no owning
    // mod. A file a mod folder already covers is skipped here - the ownership
    // join (GamePlugin::owner_mod) decides which row highlights for shadowed
    // strays instead. Game-less instances skip the whole block: native_dir
    // would be a CWD-relative path (Workspace-wk8).
    if (!request.game_dir.empty()) {
        auto native_plugins = engine::native_plugins_csv(knowledge, game_id);
        if (!native_plugins.empty()) {
            std::filesystem::path native_dir = engine::resolve_game_mods_dir(
                game_id, request.game_dir, knowledge,
                request.game_mods_dir.string());

            std::unordered_set<std::string> existing;
            for (const auto& m : scanned)
                existing.insert(m.folder_name);

            std::unordered_set<std::string> declared_native;
            std::istringstream ss(native_plugins);
            std::string plugin;
            while (std::getline(ss, plugin, ',')) {
                auto start = plugin.find_first_not_of(" \t");
                auto end = plugin.find_last_not_of(" \t");
                if (start == std::string::npos) continue;
                plugin = plugin.substr(start, end - start + 1);
                declared_native.insert(plugin);
                if (existing.count(plugin)) continue;

                auto plugin_path = native_dir / plugin;
                if (!std::filesystem::exists(plugin_path)) continue;

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
                    for (const auto& [target, source] :
                         engine::load_deploy_ledger(request.ledger_file)) {
                        (void)source;
                        if (!engine::is_plugin_file(target)) continue;
                        std::error_code cec;
                        auto canon = std::filesystem::weakly_canonical(target, cec);
                        if (!cec) deployed_plugins.insert(std::move(canon));
                    }
                }

                for (const auto& entry :
                     std::filesystem::directory_iterator(native_dir, scan_ec)) {
                    // Deployed .esp files are always symlinks (only executables
                    // are copied as real files, and .esp is never executable):
                    // skip symlinks outright. A real file the user dropped in
                    // still flows through to the unmanaged synthesis below.
                    std::error_code sec;
                    if (entry.is_symlink(sec)) continue;
                    if (!entry.is_regular_file(scan_ec)) continue;
                    if (!engine::is_plugin_file(entry.path())) continue;
                    const std::string file = entry.path().filename().string();
                    if (declared_native.count(file) || existing.count(file)) continue;
                    if (!deployed_plugins.empty()) {
                        std::error_code cec;
                        auto canon = std::filesystem::weakly_canonical(entry.path(), cec);
                        if (!cec && deployed_plugins.count(canon)) continue;
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
            std::filesystem::path native_dir = engine::resolve_game_mods_dir(
                game_id, request.game_dir, knowledge,
                request.game_mods_dir.string());

            std::unordered_set<std::string> existing;
            for (const auto& m : scanned)
                existing.insert(m.folder_name);

            for (const auto& name : unmanaged) {
                if (name.empty() || existing.count(name)) continue;
                std::error_code ec;
                if (!std::filesystem::exists(native_dir / name, ec)) continue;
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

        for (const auto& entry : std::filesystem::directory_iterator(request.mods_dir)) {
            if (!entry.is_directory()) continue;
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
                engine::Logger::instance().debug(
                    "Imported MO2 meta: " + folder_name + "/meta.ini");
            } else {
                engine::Logger::instance().warn(
                    "Failed to save imported meta: " + folder_name);
            }
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
