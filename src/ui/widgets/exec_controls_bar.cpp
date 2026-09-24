#include "ui/widgets/exec_controls_bar.h"
#include "engine/core/log/logger.h"
#include "ui/workers/exe_icon_worker.h"

#include <QApplication>
#include <QComboBox>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QProcess>
#include <QShortcut>
#include <QStyle>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

namespace {

// Immediate icon + optional background extraction request. The wrestool
// spawn (up to 3s per .exe) used to run synchronously during pre-show
// set_game_info; now .exe cache misses show a placeholder and the real icon
// arrives via ExeIconWorker::extracted (Workspace-ai23).
struct PendingIcon {
  QIcon icon;
  bool needs_async = false;
  QString key;         // entry.path - combo match key for the async update
  QString exe_path;    // absolute exe the worker extracts
  QString cache_file;  // absolute cache destination the worker fills
};

// Synchronous placeholder for an on-disk file: mirrors the tail of
// extractExeIcon (QFileIconProvider, then the standard file icon). Fast -
// no subprocess - so it is safe on the main thread.
QIcon placeholderIcon(const QString &full_path) {
  auto provider_icon = QFileIconProvider().icon(QFileInfo(full_path));
  if (!provider_icon.isNull())
    return provider_icon;
  return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

PendingIcon resolveEntryIconFast(const ui::Executables::Entry &entry,
                                 const std::filesystem::path &game_dir,
                                 const std::filesystem::path &icon_cache_dir,
                                 const std::filesystem::path &staging_dir) {
  PendingIcon out;
  if (!entry.icon_path.isEmpty()) {
    QPixmap pix(entry.icon_path);
    if (!pix.isNull())
      out.icon = QIcon(pix);
    return out;
  }

  // Cache-first: a mod-provided executable may exist only in the deploy
  // staging dir (wiped every session) or in a merged path that is never
  // physical. Once its icon has been extracted it lives in the cache by
  // basename, so restored entries keep their icon even before any deploy.
  QString cache_file;
  if (!icon_cache_dir.empty() && !entry.path.isEmpty()) {
    auto cache_key = QFileInfo(entry.path).fileName() + ".ico";
    cache_file     = QString::fromStdString(
        (std::filesystem::path(icon_cache_dir.string()) / cache_key.toStdString())
            .string());
    QIcon cached(cache_file);
    if (!cached.isNull()) {
      out.icon = cached;
      return out;
    }
  }

  QString full;
  if (!game_dir.empty() && !entry.path.isEmpty()) {
    auto full_path =
        std::filesystem::path(game_dir.string()) / entry.path.toStdString();
    if (std::filesystem::exists(full_path)) {
      full = QString::fromStdString(full_path.string());
    } else if (!staging_dir.empty()) {
      auto staged = staging_dir / entry.path.toStdString();
      if (std::filesystem::exists(staged))
        full = QString::fromStdString(staged.string());
    }
  }
  if (full.isEmpty())
    return out;

  if (cache_file.isEmpty()) {
    // No cache dir (tests, ad-hoc bars): keep the exact old synchronous
    // behavior - the worker has nowhere to publish through.
    out.icon = ui::extractExeIcon(full, icon_cache_dir);
    return out;
  }
  if (QFileInfo(full).suffix().compare("exe", Qt::CaseInsensitive) == 0) {
    // Slow wrestool path: placeholder now, real icon via the worker.
    out.icon        = placeholderIcon(full);
    out.needs_async = true;
    out.key         = entry.path;
    out.exe_path    = full;
    out.cache_file  = cache_file;
    return out;
  }
  out.icon = placeholderIcon(full);
  return out;
}

}  // namespace

namespace ui {

QIcon extractExeIcon(const QString &exePath,
                     const std::filesystem::path &icon_cache_dir) {
  auto &log      = engine::Logger::instance();
  auto exe_std   = exePath.toStdString();
  auto cache_key = QFileInfo(exePath).fileName() + ".ico";
  auto cache_path =
      std::filesystem::path(icon_cache_dir.string()) / cache_key.toStdString();

  if (!icon_cache_dir.empty() && std::filesystem::exists(cache_path)) {
    QIcon cached(QString::fromStdString(cache_path.string()));
    if (!cached.isNull())
      return cached;
    log.debug("Icon cache file exists but failed to load, re-extracting: " +
              cache_path.string());
  }

  if (QFileInfo(exePath).suffix().compare("exe", Qt::CaseInsensitive) == 0) {
    auto wrestool = findWrestool();
    if (wrestool.isEmpty()) {
      log.debug("wrestool not found, using QFileIconProvider fallback");
    } else {
      log.debug("Using wrestool: " + wrestool.toStdString() + " for " + exe_std);
      QTemporaryDir tmpDir;
      if (!tmpDir.isValid()) {
        log.warn("Failed to create temp dir for icon extraction: " + exe_std);
      } else {
        auto outIco = tmpDir.filePath("icon.ico");
        QProcess proc;
        proc.start(wrestool, {"-x", "-t", "14", exePath, "-o", outIco});
        if (!proc.waitForFinished(3000)) {
          log.warn("wrestool timed out for: " + exe_std);
        } else if (proc.exitCode() != 0) {
          auto stderr_out = QString::fromUtf8(proc.readAllStandardError()).trimmed();
          log.warn("wrestool failed for " + exe_std + " (exit " +
                   std::to_string(proc.exitCode()) + "): " + stderr_out.toStdString());
        } else {
          QIcon ico(outIco);
          if (ico.isNull()) {
            log.warn("wrestool produced ico but QIcon failed to load: " + exe_std);
          } else {
            log.debug("Icon extracted via wrestool: " + exe_std);
            if (!icon_cache_dir.empty()) {
              std::error_code ec;
              std::filesystem::create_directories(icon_cache_dir, ec);
              std::filesystem::copy_file(
                  outIco.toStdString(), cache_path.string(),
                  std::filesystem::copy_options::overwrite_existing, ec);
              if (ec) {
                log.warn("Failed to cache icon to " + cache_path.string() + ": " +
                         ec.message());
              } else {
                log.debug("Icon cached to: " + cache_path.string());
              }
            }
            return ico;
          }
        }
      }
    }
  }

  auto provider_icon = QFileIconProvider().icon(QFileInfo(exePath));
  if (!provider_icon.isNull())
    return provider_icon;
  return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

ExecControlsBar::ExecControlsBar(QWidget *parent) : QWidget(parent) {
  auto *layout = new QGridLayout(this);
  layout->setContentsMargins(4, 2, 4, 2);
  layout->setSpacing(4);

  exec_combo_ = new QComboBox(this);
  exec_combo_->setMinimumHeight(50);
  exec_combo_->setMinimumWidth(200);
  exec_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  exec_combo_->addItem(tr(kAddNewEntryText));
  layout->addWidget(exec_combo_, 0, 0, 2, 1);

  run_btn_ = new QToolButton(this);
  run_btn_->setText(tr("&Run"));
  run_btn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
  run_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  run_btn_->setMinimumHeight(24);
  run_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout->addWidget(run_btn_, 0, 1);

  shortcut_btn_ = new QToolButton(this);
  shortcut_btn_->setText(tr("S&hortcut"));
  shortcut_btn_->setIcon(style()->standardIcon(QStyle::SP_FileLinkIcon));
  shortcut_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  shortcut_btn_->setMinimumHeight(24);
  shortcut_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  shortcut_btn_->setPopupMode(QToolButton::MenuButtonPopup);

  auto *shortcut_menu = new QMenu(this);
  shortcut_menu->addAction(tr("Shortcut to Toolbar"));
  shortcut_menu->addAction(tr("Shortcut to Desktop"));
  connect(shortcut_menu->actions()[0], &QAction::triggered, this,
          &ExecControlsBar::shortcut_to_toolbar);
  connect(shortcut_menu->actions()[1], &QAction::triggered, this,
          &ExecControlsBar::shortcut_to_desktop);
  shortcut_btn_->setMenu(shortcut_menu);
  layout->addWidget(shortcut_btn_, 1, 1);

  layout->setColumnStretch(0, 7);
  layout->setColumnStretch(1, 3);

  connect(run_btn_, &QToolButton::clicked, this, &ExecControlsBar::run_clicked);
  connect(shortcut_btn_, &QToolButton::clicked, this,
          &ExecControlsBar::shortcut_to_toolbar);

  // Keyboard-only navigation shortcuts (Workspace-w35b)
  run_shortcut_ = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_R), this);
  connect(run_shortcut_, &QShortcut::activated, this, &ExecControlsBar::run_clicked);

  shortcut_menu_shortcut_ = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_S), this);
  connect(shortcut_menu_shortcut_, &QShortcut::activated, this, [this]() {
    // Show the shortcut button's dropdown menu at its position
    if (shortcut_btn_->menu()) {
      shortcut_btn_->menu()->popup(
          shortcut_btn_->mapToGlobal(QPoint(0, shortcut_btn_->height())));
    }
  });

  // When "<Edit...>" ends up selected, snap back to the previous real
  // selection and open the executable editor. The editor is wired here
  // (not only in activated) because Qt's hidePopup() emits
  // activated(currentIndex()) *after* setCurrentIndex, which means the
  // currentIndexChanged handler's restore may shift currentIndex before
  // activated fires - making activated()'s index parameter unreliable for
  // detecting the sentinel in the entries-present case.
  connect(exec_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
    auto &log = engine::Logger::instance();
    if (index < 0)
      return;
    if (exec_combo_->itemData(index).toJsonObject().isEmpty()) {
      // Sentinel (index 0). Keep the combo on the last real selection,
      // NOT the entry right after the sentinel, and don't report this
      // artificial restore as a user selection change - it would
      // clobber the in-memory selection with the wrong executable. With
      // no real entries there is nothing to restore.
      log.debug("ExecControlsBar: sentinel selected (index 0), count=" +
                std::to_string(exec_combo_->count()));
      if (exec_combo_->count() > 1) {
        int restore = (last_real_index_ >= 1 && last_real_index_ < exec_combo_->count())
                          ? last_real_index_
                          : 1;
        log.debug("ExecControlsBar: restoring combo to index " +
                  std::to_string(restore));
        {
          QSignalBlocker blocker(exec_combo_);
          exec_combo_->setCurrentIndex(restore);
        }
      }
      log.debug("ExecControlsBar: emitting add_entry_requested from "
                "currentIndexChanged");
      emit add_entry_requested();
      return;
    }
    if (index >= 1)
      last_real_index_ = index;
    // Report only real selections. The lone sentinel (empty combo after
    // clear_executables) must not be reported - it would overwrite a
    // persisted selection before the real entries are populated.
    if (!current_executable().isEmpty())
      emit current_executable_changed();
  });
  // activated() fires only for user interaction (never programmatic), making
  // it the right place to catch the 0-executables edge case: the sentinel
  // is already current so currentIndexChanged never fires, and only
  // activated() will. For entries-present, currentIndexChanged above already
  // emitted add_entry_requested(), so guard with current_executable().isEmpty()
  // to avoid a double-fire.
  connect(exec_combo_, &QComboBox::activated, this, [this](int index) {
    if (index == 0 && current_executable().isEmpty()) {
      engine::Logger::instance().debug(
          "ExecControlsBar: activated sentinel in 0-executables fallback");
      emit add_entry_requested();
    }
  });
}

ExecControlsBar::~ExecControlsBar() {
  // Bounded wait: an in-flight wrestool run finishes on its own (3s
  // timeout), so this only blocks shutdown briefly - the same trade the
  // DeployThread owner accepts. The worker is never left running into a
  // dead receiver.
  if (icon_thread_ != nullptr) {
    icon_thread_->quit();
    icon_thread_->wait(5000);
  }
}

void ExecControlsBar::ensure_icon_worker() {
  if (icon_worker_ != nullptr)
    return;
  icon_thread_ = new QThread(this);
  icon_worker_ = new ExeIconWorker();
  icon_worker_->moveToThread(icon_thread_);
  connect(icon_thread_, &QThread::finished, icon_worker_, &QObject::deleteLater);
  connect(this, &ExecControlsBar::request_extraction, icon_worker_,
          &ExeIconWorker::extract);
  connect(icon_worker_, &ExeIconWorker::extracted, this,
          &ExecControlsBar::on_exe_icon_ready);
  icon_thread_->start();
}

void ExecControlsBar::request_async_icon(const QString &key, const QString &exe_path,
                                         const QString &cache_file) {
  if (key.isEmpty() || exe_path.isEmpty() || cache_file.isEmpty())
    return;
  ensure_icon_worker();
  emit request_extraction(key, exe_path, cache_file, icon_generation_);
}

void ExecControlsBar::on_exe_icon_ready(const QString &key, const QString &cache_file,
                                        bool ok, quint64 ticket) {
  // Stale (a rebuild bumped the generation) or failed (wrestool missing /
  // error): keep the placeholder - it is the same QFileIconProvider icon
  // the synchronous path would have produced.
  if (!ok || ticket != icon_generation_)
    return;
  QIcon ico(cache_file);
  if (ico.isNull())
    return;
  for (int i = 1; i < exec_combo_->count(); ++i) {
    if (item_data(i)["path"].toString() == key)
      exec_combo_->setItemIcon(i, ico);
  }
}

QJsonObject ExecControlsBar::item_data(int index) const {
  auto var = exec_combo_->itemData(index);
  if (var.userType() == QMetaType::QJsonObject)
    return var.toJsonObject();
  // Legacy: plain string path
  QString s = var.toString();
  if (!s.isEmpty())
    return Executables::Entry::fromLegacyPath(s).toJson();
  return {};
}

void ExecControlsBar::set_item_data(int index, const QJsonObject &obj) {
  exec_combo_->setItemData(index, QVariant(obj));
}

QString ExecControlsBar::current_executable() const {
  auto obj = item_data(exec_combo_->currentIndex());
  return obj["path"].toString();
}

int ExecControlsBar::current_executable_index() const {
  return exec_combo_->currentIndex();
}

QStringList ExecControlsBar::executable_paths() const {
  QStringList paths;
  for (int i = 1; i < exec_combo_->count(); ++i) {
    auto obj = item_data(i);
    auto p   = obj["path"].toString();
    if (!p.isEmpty())
      paths.append(p);
  }
  return paths;
}

QVector<Executables::Entry> ExecControlsBar::executable_entries() const {
  QVector<Executables::Entry> entries;
  for (int i = 1; i < exec_combo_->count(); ++i) {
    auto obj = item_data(i);
    if (!obj.isEmpty())
      entries.append(Executables::Entry::fromJson(obj));
  }
  return entries;
}

void ExecControlsBar::add_executable(const QString &display_name,
                                     const QString &rel_path, const QIcon &icon) {
  Executables::Entry e;
  e.title        = display_name;
  e.path         = rel_path;
  int insert_pos = 1;  // right after the sentinel (index 0)
  exec_combo_->insertItem(insert_pos, icon, Executables::exec_entry_display_name(e),
                          QVariant(e.toJson()));
  exec_combo_->setCurrentIndex(insert_pos);
}

void ExecControlsBar::add_entry(const Executables::Entry &entry) {
  auto pending = resolveEntryIconFast(entry, game_dir_, icon_cache_dir_, staging_dir_);

  // Append at the end (after the sentinel) so the combo order matches the
  // order entries were added in - full rebuilds must not reverse the list.
  int insert_pos = exec_combo_->count();
  exec_combo_->insertItem(insert_pos, pending.icon,
                          Executables::exec_entry_display_name(entry),
                          QVariant(entry.toJson()));
  exec_combo_->setCurrentIndex(insert_pos);
  if (pending.needs_async)
    request_async_icon(pending.key, pending.exe_path, pending.cache_file);
}

Executables::Entry ExecControlsBar::current_entry() const {
  int idx = exec_combo_->currentIndex();
  if (idx <= 0)  // index 0 is the sentinel, not an executable
    return {};
  return Executables::Entry::fromJson(item_data(idx));
}

void ExecControlsBar::clear_executables() {
  // Programmatic rebuild: suppress currentIndexChanged so re-adding the
  // sentinel never looks like a selection change.
  QSignalBlocker blocker(exec_combo_);
  exec_combo_->clear();
  // Re-add the sentinel so add_entry() works
  exec_combo_->addItem(tr(kAddNewEntryText), QVariant(QJsonObject()));
  // The list is gone: any in-flight background extraction is stale.
  ++icon_generation_;
}

bool ExecControlsBar::select_executable(const QString &path) {
  if (path.isEmpty())
    return false;
  for (int i = 1; i < exec_combo_->count(); ++i) {
    if (item_data(i)["path"].toString() == path) {
      exec_combo_->setCurrentIndex(i);
      return true;
    }
  }
  return false;
}

void ExecControlsBar::set_executables(const QStringList &names,
                                      const QString &default_name,
                                      const std::filesystem::path &game_dir,
                                      const std::filesystem::path &icon_cache_dir,
                                      const std::filesystem::path &staging_dir) {
  // Programmatic rebuild: suppress currentIndexChanged while resetting to
  // the bare sentinel so it never looks like a selection change.
  {
    QSignalBlocker blocker(exec_combo_);
    exec_combo_->clear();
    // Sentinel stays first (index 0); real executables are appended after it
    exec_combo_->addItem(tr(kAddNewEntryText), QVariant(QJsonObject()));
  }
  // New generation: in-flight background extractions from the previous
  // population are stale and dropped in on_exe_icon_ready (Workspace-ai23).
  ++icon_generation_;

  // Keep the resolution context for later add_entry() calls (dialog accept,
  // missing-exe prune) so every rebuild path resolves icons identically.
  game_dir_       = game_dir;
  icon_cache_dir_ = icon_cache_dir;
  staging_dir_    = staging_dir;

  for (int i = 0; i < names.size(); ++i) {
    auto raw = names[i];

    // Detect JSON string vs plain legacy path
    Executables::Entry entry;
    if (raw.startsWith('{')) {
      QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
      if (doc.isObject()) {
        entry = Executables::Entry::fromJson(doc.object());
      } else {
        entry = Executables::Entry::fromLegacyPath(raw);
      }
    } else {
      entry = Executables::Entry::fromLegacyPath(raw);
    }

    auto display = Executables::exec_entry_display_name(entry);
    // Placeholder icon now, real .exe icon via the background worker: the
    // combo (and the window) shows immediately instead of blocking up to
    // 3s per .exe on wrestool (Workspace-ai23).
    auto pending =
        resolveEntryIconFast(entry, game_dir_, icon_cache_dir_, staging_dir_);
    exec_combo_->addItem(pending.icon, display, QVariant(entry.toJson()));
    if (pending.needs_async)
      request_async_icon(pending.key, pending.exe_path, pending.cache_file);
  }

  if (!default_name.isEmpty()) {
    for (int i = 1; i < exec_combo_->count(); ++i) {
      auto p = item_data(i)["path"].toString();
      if (!p.isEmpty() && p == default_name) {
        exec_combo_->setCurrentIndex(i);
        return;
      }
    }
  }
  // No default match (or none requested): land on the first real executable
  if (exec_combo_->count() > 1)
    exec_combo_->setCurrentIndex(1);
}

}  // namespace ui
