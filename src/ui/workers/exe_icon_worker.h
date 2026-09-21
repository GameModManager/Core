#pragma once

#include <QObject>
#include <QString>

namespace ui {

// Absolute path of the wrestool binary (bundled next to the app, else
// $PATH). Empty when not found. Shared by the synchronous single-shot
// extractExeIcon path and the background worker below.
QString findWrestool();

// Background .exe icon extractor (Workspace-ai23). The wrestool QProcess
// spawn + up-to-3s wait used to run synchronously inside pre-show
// set_game_info, blocking the main thread before the window appeared. Now it
// runs here, on a QThread owned by ExecControlsBar: the combo shows a
// placeholder icon immediately and swaps in the real icon when `extracted`
// fires.
//
// Threading: only filesystem + raw bytes on this thread. QIcon/QPixmap
// construction stays on the GUI thread in the receiver (QPixmap is not
// thread-safe), so the worker reports the on-disk cache path and the
// receiver builds the QIcon itself.
class ExeIconWorker : public QObject {
  Q_OBJECT
public:
  explicit ExeIconWorker(QObject* parent = nullptr);

public slots:
  // Extracts the icon for exe_path via wrestool and copies the result to
  // cache_file_path (creating parent dirs). Emits extracted() with ok=false
  // when wrestool is missing, fails, or times out - the receiver keeps the
  // placeholder. key/ticket are echoed back so the receiver can match the
  // result to a combo item and drop stale results after a rebuild.
  void extract(const QString& key, const QString& exe_path,
               const QString& cache_file_path, quint64 ticket);

signals:
  void extracted(const QString& key, const QString& cache_file_path, bool ok,
                 quint64 ticket);
};

}  // namespace ui
