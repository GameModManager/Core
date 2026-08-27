#include "ui/main_window/saves_scan_worker.h"

#include "engine/game/saves/save_reader.h"
#include "engine/game/saves/save_scanner.h"
#include "engine/pipeline/plugin_host/save_parser_registry.h"

#include <QMetaObject>
#include <QThread>

#include <utility>
namespace ui {

SavesScanWorker::SavesScanWorker(QObject* parent) : QObject(parent) {}

void SavesScanWorker::run(SavesScanRequest request) {
    SavesScanResult result;
    result.saves_dir = request.saves_dir;
    if (!request.saves_dir.empty() &&
        std::filesystem::is_directory(request.saves_dir)) {
        /* Resolve the save parser from the save-parser registry (populated by
         * v1/v2 plugins and the engine builtins). Falls back to a no-op parser
         * that returns empty saves when no parser is registered for this game. */
        std::string game_id = request.game_id;
        auto parses = engine::SaveParseFn{};
        if (engine::SaveParserRegistry::instance().has_parser(game_id)) {
            parses = [game_id](const std::filesystem::path& p) -> engine::SaveGame {
                auto result =
                    engine::SaveParserRegistry::instance().parse_save(p, game_id);
                if (!result) {
                    throw engine::SaveParseError("no save parser for " + game_id);
                }
                return *result;
            };
        }
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
