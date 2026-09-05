#include "ui/main_window/saves_scan_worker.h"

#include "engine/game/saves/save_reader.h"
#include "engine/game/saves/save_scanner.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <QMetaObject>
#include <QThread>

#include <chrono>
#include <utility>
namespace ui {

SavesScanWorker::SavesScanWorker(QObject* parent) : QObject(parent) {}

void SavesScanWorker::run(SavesScanRequest request) {
    SavesScanResult result;
    result.saves_dir = request.saves_dir;
    if (!request.saves_dir.empty() &&
        std::filesystem::is_directory(request.saves_dir)) {
        /* Resolve the save parser from the save-parser registry (populated by
         * v1/v2 plugins and the engine builtins). When no parser is registered
         * for this game, fall back to a stub that emits a SaveGame carrying
         * just the file path and filesystem mtime - the Saves tab still lists
         * the file (newest first) and the missing-asset column stays empty.
         * An empty std::function here would make scan_saves skip every file
         * (it guards against std::bad_function_call), leaving the tab silent
         * for any game without a registered parser. */
        std::string game_id = request.game_id;
        engine::SaveParseFn parses =
            [game_id](const std::filesystem::path& p) -> engine::SaveGame {
            if (engine::SaveParserRegistry::instance().has_parser(game_id)) {
                auto r =
                    engine::SaveParserRegistry::instance().parse_save(p, game_id);
                if (!r) {
                    throw engine::SaveParseError("no save parser for " + game_id);
                }
                return *r;
            }
            // No parser for this game_id - still list the file so the user
            // sees something. The parser may not be loaded (plugin missing,
            // game plugin hasn't shipped one) and an empty tab is worse than
            // a row with no metadata. Mtime → epoch via the standard
            // clock_cast pair (filesystem clock is not Unix epoch on
            // libstdc++): see engine/game/detect/mod_scanner.cpp for the
            // same pattern.
            engine::SaveGame stub;
            stub.file_path = p;
            stub.game_id = game_id;
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(p, ec);
            if (!ec) {
                stub.creation_time = static_cast<engine::SaveEpochSeconds>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::clock_cast<std::chrono::system_clock>(mtime)
                            .time_since_epoch())
                        .count());
            }
            return stub;
        };
        auto saves = engine::scan_saves(
            request.saves_dir, request.extensions, parses);
        for (auto& save : saves) {
            SavesScanResultEntry entry;
            entry.save = std::move(save);
            entry.missing = engine::find_save_missing_assets(
                entry.save, request.plugins, request.mods_dir,
                request.overwrite_dir);
            result.entries.push_back(std::move(entry));
        }
    }
    emit finished(std::move(result));
}

SavesScanThread::SavesScanThread(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ui::SavesScanResultEntry>();
    qRegisterMetaType<ui::SavesScanResult>();
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-saves-scan"));
    worker_ = new SavesScanWorker(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(thread_, &QThread::finished, this, &SavesScanThread::operation_finished);
    thread_->start();
}

SavesScanThread::~SavesScanThread() {
    thread_->quit();
    thread_->wait();
}

void SavesScanThread::start(SavesScanRequest request) {
    SavesScanWorker* worker = worker_;
    QMetaObject::invokeMethod(
        worker,
        [worker, req = std::move(request)]() mutable {
            worker->run(std::move(req));
        },
        Qt::QueuedConnection);
}

}  // namespace ui
