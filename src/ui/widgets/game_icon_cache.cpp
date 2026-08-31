#include "ui/widgets/game_icon_cache.h"

#include "engine/core/instance/game_icons.h"
#include "engine/core/log/logger.h"

#include <QMetaObject>
#include <QPainter>
#include <QPixmap>
#include <QThread>

#include <filesystem>

namespace fs = std::filesystem;

namespace ui {

GameIconFetcher::GameIconFetcher(QObject* parent) : QObject(parent) {}

void GameIconFetcher::run(const QString& game_id, const QString& url) {
    std::string error;
    bool ok = engine::ensure_icon_cached(game_id.toStdString(),
                                         url.toStdString(), error);
    if (!ok) {
        engine::Logger::instance().warn("Icon fetch failed for " +
                                        game_id.toStdString() + ": " + error);
    }
    emit finished(game_id, ok);
}

namespace {

// Enforce an exact square target size: scale keeping aspect ratio and center
// on a transparent square canvas (declared icons may arrive at any size).
QPixmap enforce_size(const QPixmap& src, int target) {
    if (src.isNull()) return {};
    if (src.width() == target && src.height() == target) return src;
    QPixmap scaled = src.scaled(target, target, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    if (scaled.width() == target && scaled.height() == target) return scaled;
    QPixmap canvas(target, target);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawPixmap((target - scaled.width()) / 2,
                 (target - scaled.height()) / 2, scaled);
    return canvas;
}

}  // namespace

GameIconCache& GameIconCache::instance() {
    static GameIconCache cache;
    return cache;
}

void GameIconCache::set_knowledge(const engine::GameKnowledge* knowledge) {
    instance().knowledge_ = knowledge;
}

GameIconCache::GameIconCache() {
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("gmm-icon-fetch"));
    worker_ = new GameIconFetcher(nullptr);
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &GameIconFetcher::finished,
            this, &GameIconCache::on_fetch_finished);
    thread_->start();
}

GameIconCache::~GameIconCache() {
    thread_->quit();
    thread_->wait();
}

QIcon GameIconCache::icon_for(const QString& game_id, const QString& name,
                              int size) {
    std::string gid = game_id.toStdString();

    std::string url =
        knowledge_ ? engine::icon_url_for(*knowledge_, gid) : std::string();
    if (!url.empty()) {
        auto cached = engine::cached_icon_path(gid);
        std::error_code ec;
        bool cached_ok =
            fs::is_regular_file(cached, ec) && !ec &&
            fs::file_size(cached, ec) > 0;
        if (cached_ok) {
            QPixmap pm(QString::fromStdString(cached.string()));
            if (!pm.isNull()) return QIcon(enforce_size(pm, size));
        }
        // Not cached (or unreadable): fetch in the background, show the avatar
        // for now - icon_ready() fires when the download lands.
        request_fetch(gid, url);
    }
    return make_placeholder(game_id, name, size);
}

void GameIconCache::request_fetch(const std::string& game_id,
                                  const std::string& url) {
    if (pending_.count(game_id)) return;  // already queued
    pending_.insert(game_id);

    GameIconFetcher* worker = worker_;
    QString gid = QString::fromStdString(game_id);
    QString u = QString::fromStdString(url);
    QMetaObject::invokeMethod(
        worker,
        [worker, gid, u]() { worker->run(gid, u); },
        Qt::QueuedConnection);
}

void GameIconCache::on_fetch_finished(const QString& game_id, bool ok) {
    pending_.erase(game_id.toStdString());
    if (ok) emit icon_ready(game_id);
}

QIcon GameIconCache::make_placeholder(const QString& game_id,
                                      const QString& name, int size) {
    auto hash = std::hash<std::string>{}(game_id.toStdString());
    QColor base;
    switch (hash % 8) {
        case 0: base = QColor(100, 149, 237); break;  // cornflower blue
        case 1: base = QColor(220, 80, 80);   break;  // red
        case 2: base = QColor(80, 180, 100);  break;  // green
        case 3: base = QColor(200, 160, 60);  break;  // gold
        case 4: base = QColor(160, 100, 200); break;  // purple
        case 5: base = QColor(60, 180, 200);  break;  // teal
        case 6: base = QColor(220, 140, 60);  break;  // orange
        case 7: base = QColor(120, 120, 180); break;  // slate
    }

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Circle
    p.setPen(Qt::NoPen);
    p.setBrush(base);
    p.drawEllipse(1, 1, size - 2, size - 2);

    // First letter of the display name
    QString letter;
    if (!name.isEmpty())
        letter = name.left(1).toUpper();
    else
        letter = game_id.left(1).toUpper();

    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPointSize(size >= 48 ? 24 : 18);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, letter);

    return QIcon(pm);
}

}  // namespace ui
