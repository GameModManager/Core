#pragma once

#include "engine/game/registry/game_knowledge.h"

#include <QIcon>
#include <QObject>
#include <QString>

#include <string>
#include <unordered_set>

class QThread;

namespace ui {

// Worker that runs on the gmm-icon-fetch thread. Downloads a game icon into
// the global cache (Qt-free work: curl + filesystem only - never touches a
// widget). One fetch per invocation; the thread owns no mutable state the UI
// thread could race with.
class GameIconFetcher : public QObject {
    Q_OBJECT
public:
    explicit GameIconFetcher(QObject* parent = nullptr);

    // Runs on the worker thread. Only ever invoked through GameIconCache.
    void run(const QString& game_id, const QString& url);

signals:
    void finished(const QString& game_id, bool ok);
};

// Main-thread facade for game icons (instance switcher rows + game selection
// cards). Resolves an icon synchronously from the global cache - or a built-in
// letter avatar while a download is in flight - and fetches missing icons on a
// dedicated worker thread, emitting icon_ready(game_id) when a download lands
// so widgets can re-resolve. Singleton so the first-run screen and the instance
// switcher share one cache and one worker thread.
class GameIconCache : public QObject {
    Q_OBJECT
public:
    static GameIconCache& instance();

    // Point at the game knowledge (called once from main.cpp after plugins
    // load; the store outlives the GUI). Icon URLs are read from it.
    static void set_knowledge(const engine::GameKnowledge* knowledge);

    // Icon for a game at an exact square size, dimensions enforced. Falls back
    // to a colored-circle avatar when no icon is declared, not cached yet, or
    // not downloadable.
    QIcon icon_for(const QString& game_id, const QString& name, int size);

signals:
    void icon_ready(const QString& game_id);

private:
    GameIconCache();
    ~GameIconCache() override;

    void request_fetch(const std::string& game_id, const std::string& url);
    void on_fetch_finished(const QString& game_id, bool ok);

    static QIcon make_placeholder(const QString& game_id,
                                  const QString& name,
                                  int size);

    const engine::GameKnowledge* knowledge_ = nullptr;
    QThread* thread_ = nullptr;
    GameIconFetcher* worker_ = nullptr;
    std::unordered_set<std::string> pending_;
};

}  // namespace ui
