#include "ui/main_window/mod_scan_worker.h"

#include "engine/log/logger.h"
#include "engine/meta/mod_meta.h"
#include "engine/plugins/plugin_database.h"

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

    // Scan game's native mods directory.
    scanned = engine::ModScanner::scan(knowledge, game_id, request.game_dir,
                                       ignore_symlink_targets);

    // Instance mode: the mod list comes from the instance mods dir ONLY. The
    // game's own mods subpath is never a mod source - its folders (e.g.
    // Skyrim's Data/Scripts, Data/Video) are vanilla game content, not mods,
    // and would otherwise be listed (and flagged) as mods. MO2 lists only
    // <instance>/mods.
    if (!request.instance_root.empty()) {
        auto game_mods_subpath = knowledge.get(game_id, "mods_subpath", "");
        auto game_mods_dir = request.game_dir / game_mods_subpath;
        // Only scan separately if they're different directories
        std::error_code ec_canon;
        auto inst_canon = std::filesystem::weakly_canonical(request.mods_dir, ec_canon);
        auto game_canon = std::filesystem::weakly_canonical(game_mods_dir, ec_canon);
        if (inst_canon != game_canon) {
            scanned = engine::ModScanner::scan_dir(knowledge, game_id, request.mods_dir,
                                                   std::vector<std::filesystem::path>{});
        }
    }

    // Detect game-native plugins (e.g. vanilla ESMs) from the game's mods
    // directory, and synthesize unmanaged rows for stray plugins dropped
    // straight into the game's Data dir (MO2's UnmanagedMods behavior) so the
    // mod<->plugin selection highlight round-trips for files with no owning
    // mod. A file a mod folder already covers is skipped here - the ownership
    // join (GamePlugin::owner_mod) decides which row highlights for shadowed
    // strays instead.
    {
        auto native_plugins_csv = knowledge.get(game_id, "game_native_plugins", "");
        if (!native_plugins_csv.empty()) {
            auto game_mods_subpath = knowledge.get(game_id, "mods_subpath", "");
            std::filesystem::path native_dir = request.game_dir;
            if (!game_mods_subpath.empty())
                native_dir /= game_mods_subpath;

            std::unordered_set<std::string> existing;
            for (const auto& m : scanned)
                existing.insert(m.folder_name);

            std::unordered_set<std::string> declared_native;
            std::istringstream ss(native_plugins_csv);
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
                for (const auto& entry :
                     std::filesystem::directory_iterator(native_dir, scan_ec)) {
                    if (!entry.is_regular_file(scan_ec)) continue;
                    if (!engine::is_plugin_file(entry.path())) continue;
                    const std::string file = entry.path().filename().string();
                    if (declared_native.count(file) || existing.count(file)) continue;
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
