#include "ui/workers/exe_icon_worker.h"

#include "engine/core/log/logger.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <filesystem>

namespace ui {

QString findWrestool() {
  auto app_dir = QCoreApplication::applicationDirPath();
  auto bundled = app_dir + "/../tools/linux/wrestool";
  if (QFileInfo::exists(bundled))
    return bundled;
  auto system = QStandardPaths::findExecutable("wrestool");
  if (!system.isEmpty())
    return system;
  return {};
}

ExeIconWorker::ExeIconWorker(QObject *parent) : QObject(parent) {}

void ExeIconWorker::extract(const QString &key, const QString &exe_path,
                            const QString &cache_file_path, quint64 ticket) {
  auto &log          = engine::Logger::instance();
  const auto exe_std = exe_path.toStdString();
  bool ok            = false;

  const auto wrestool = findWrestool();
  if (wrestool.isEmpty()) {
    log.debug("ExeIconWorker: wrestool not found, keeping placeholder for " + exe_std);
  } else {
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
      log.warn("ExeIconWorker: temp dir failed for " + exe_std);
    } else {
      const auto outIco = tmpDir.filePath("icon.ico");
      QProcess proc;
      proc.start(wrestool, {"-x", "-t", "14", exe_path, "-o", outIco});
      if (!proc.waitForFinished(3000)) {
        log.warn("ExeIconWorker: wrestool timed out for " + exe_std);
        proc.kill();
      } else if (proc.exitCode() != 0) {
        const auto stderr_out =
            QString::fromUtf8(proc.readAllStandardError()).trimmed();
        log.warn("ExeIconWorker: wrestool failed for " + exe_std + " (exit " +
                 std::to_string(proc.exitCode()) + "): " + stderr_out.toStdString());
      } else {
        // QImage (not QIcon/QPixmap) for the validity check: image loading
        // is thread-safe, pixmaps are GUI-thread-only. The receiver builds
        // the real QIcon from the cache file.
        const QImage img(outIco);
        if (img.isNull()) {
          log.warn("ExeIconWorker: wrestool produced an unloadable ico for " + exe_std);
        } else {
          std::error_code ec;
          const auto cache_std = cache_file_path.toStdString();
          std::filesystem::create_directories(
              std::filesystem::path(cache_std).parent_path(), ec);
          if (ec) {
            log.warn("ExeIconWorker: cannot create icon cache dir for " + cache_std +
                     ": " + ec.message());
          } else {
            std::filesystem::copy_file(
                outIco.toStdString(), cache_std,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
              log.warn("ExeIconWorker: cannot cache icon to " + cache_std + ": " +
                       ec.message());
            } else {
              log.debug("ExeIconWorker: icon cached to " + cache_std);
              ok = true;
            }
          }
        }
      }
    }
  }

  emit extracted(key, cache_file_path, ok, ticket);
}

}  // namespace ui
