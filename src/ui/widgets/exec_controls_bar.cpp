#include "ui/widgets/exec_controls_bar.h"
#include "engine/core/log/logger.h"

#include <QApplication>
#include <QComboBox>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QToolButton>

namespace {

QString findWrestool() {
    auto app_dir = QCoreApplication::applicationDirPath();
    auto bundled = app_dir + "/../tools/linux/wrestool";
    if (QFileInfo::exists(bundled)) return bundled;
    auto system = QStandardPaths::findExecutable("wrestool");
    if (!system.isEmpty()) return system;
    return {};
}

QIcon resolveEntryIcon(const ui::ExecEntry& entry,
                       const std::filesystem::path& game_dir,
                       const std::filesystem::path& icon_cache_dir,
                       const std::filesystem::path& staging_dir) {
    if (!entry.icon_path.isEmpty()) {
        QPixmap pix(entry.icon_path);
        if (!pix.isNull())
            return QIcon(pix);
    }

    // Cache-first: a mod-provided executable may exist only in the deploy
    // staging dir (wiped every session) or in a merged path that is never
    // physical. Once its icon has been extracted it lives in the cache by
    // basename, so restored entries keep their icon even before any deploy.
    if (!icon_cache_dir.empty() && !entry.path.isEmpty()) {
        auto cache_key = QFileInfo(entry.path).fileName() + ".ico";
        auto cache_path = std::filesystem::path(icon_cache_dir.string()) / cache_key.toStdString();
        QIcon cached(QString::fromStdString(cache_path.string()));
        if (!cached.isNull())
            return cached;
    }

    if (!game_dir.empty() && !entry.path.isEmpty()) {
        auto full_path = std::filesystem::path(game_dir.string()) / entry.path.toStdString();
        if (std::filesystem::exists(full_path)) {
            return ui::extractExeIcon(QString::fromStdString(full_path.string()), icon_cache_dir);
        }
        if (!staging_dir.empty()) {
            auto staged = staging_dir / entry.path.toStdString();
            if (std::filesystem::exists(staged)) {
                return ui::extractExeIcon(QString::fromStdString(staged.string()), icon_cache_dir);
            }
        }
    }
    return {};
}

}  // namespace

namespace ui {

QIcon extractExeIcon(const QString& exePath, const std::filesystem::path& icon_cache_dir) {
    auto& log = engine::Logger::instance();
    auto exe_std = exePath.toStdString();
    auto cache_key = QFileInfo(exePath).fileName() + ".ico";
    auto cache_path = std::filesystem::path(icon_cache_dir.string()) / cache_key.toStdString();

    if (!icon_cache_dir.empty() && std::filesystem::exists(cache_path)) {
        QIcon cached(QString::fromStdString(cache_path.string()));
        if (!cached.isNull()) return cached;
        log.debug("Icon cache file exists but failed to load, re-extracting: " + cache_path.string());
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
                                log.warn("Failed to cache icon to " + cache_path.string() + ": " + ec.message());
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
    if (!provider_icon.isNull()) return provider_icon;
    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

ExecControlsBar::ExecControlsBar(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    exec_combo_ = new QComboBox(this);
    exec_combo_->setMinimumHeight(50);
    exec_combo_->setMinimumWidth(200);
    exec_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    exec_combo_->addItem(tr(kAddNewEntryText));
    layout->addWidget(exec_combo_, 0, 0, 2, 1);

    run_btn_ = new QToolButton(this);
    run_btn_->setText(tr("Run"));
    run_btn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    run_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    run_btn_->setMinimumHeight(24);
    run_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(run_btn_, 0, 1);

    shortcut_btn_ = new QToolButton(this);
    shortcut_btn_->setText(tr("Shortcut"));
    shortcut_btn_->setIcon(style()->standardIcon(QStyle::SP_FileLinkIcon));
    shortcut_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    shortcut_btn_->setMinimumHeight(24);
    shortcut_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    shortcut_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* shortcut_menu = new QMenu(this);
    shortcut_menu->addAction(tr("Shortcut to Toolbar"));
    shortcut_menu->addAction(tr("Shortcut to Desktop"));
    connect(shortcut_menu->actions()[0], &QAction::triggered,
            this, &ExecControlsBar::shortcut_to_toolbar);
    connect(shortcut_menu->actions()[1], &QAction::triggered,
            this, &ExecControlsBar::shortcut_to_desktop);
    shortcut_btn_->setMenu(shortcut_menu);
    layout->addWidget(shortcut_btn_, 1, 1);

    layout->setColumnStretch(0, 7);
    layout->setColumnStretch(1, 3);

    connect(run_btn_, &QToolButton::clicked, this, &ExecControlsBar::run_clicked);
    connect(shortcut_btn_, &QToolButton::clicked,
            this, &ExecControlsBar::shortcut_to_toolbar);

    // When "<Edit...>" is chosen, emit signal and restore previous selection
    connect(exec_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        if (exec_combo_->itemData(index).toJsonObject().isEmpty()
            && exec_combo_->count() > 1) {
            // Sentinel (index 0). Keep the combo on the last real selection,
            // NOT the entry right after the sentinel, and don't report this
            // artificial restore as a user selection change - it would clobber
            // the in-memory selection with the wrong executable.
            int restore = (last_real_index_ >= 1 && last_real_index_ < exec_combo_->count())
                ? last_real_index_ : 1;
            {
                QSignalBlocker blocker(exec_combo_);
                exec_combo_->setCurrentIndex(restore);
            }
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
}

QJsonObject ExecControlsBar::item_data(int index) const {
    auto var = exec_combo_->itemData(index);
    if (var.userType() == QMetaType::QJsonObject)
        return var.toJsonObject();
    // Legacy: plain string path
    QString s = var.toString();
    if (!s.isEmpty())
        return ExecEntry::fromLegacyPath(s).toJson();
    return {};
}

void ExecControlsBar::set_item_data(int index, const QJsonObject& obj) {
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
        auto p = obj["path"].toString();
        if (!p.isEmpty())
            paths.append(p);
    }
    return paths;
}

QVector<ExecEntry> ExecControlsBar::executable_entries() const {
    QVector<ExecEntry> entries;
    for (int i = 1; i < exec_combo_->count(); ++i) {
        auto obj = item_data(i);
        if (!obj.isEmpty())
            entries.append(ExecEntry::fromJson(obj));
    }
    return entries;
}

void ExecControlsBar::add_executable(const QString& display_name, const QString& rel_path,
                                      const QIcon& icon) {
    ExecEntry e;
    e.title = display_name;
    e.path = rel_path;
    int insert_pos = 1;  // right after the sentinel (index 0)
    exec_combo_->insertItem(insert_pos, icon, exec_entry_display_name(e), QVariant(e.toJson()));
    exec_combo_->setCurrentIndex(insert_pos);
}

void ExecControlsBar::add_entry(const ExecEntry& entry) {
    QIcon icon = resolveEntryIcon(entry, game_dir_, icon_cache_dir_, staging_dir_);

    // Append at the end (after the sentinel) so the combo order matches the
    // order entries were added in - full rebuilds must not reverse the list.
    int insert_pos = exec_combo_->count();
    exec_combo_->insertItem(insert_pos, icon, exec_entry_display_name(entry), QVariant(entry.toJson()));
    exec_combo_->setCurrentIndex(insert_pos);
}

ExecEntry ExecControlsBar::current_entry() const {
    int idx = exec_combo_->currentIndex();
    if (idx <= 0)  // index 0 is the sentinel, not an executable
        return {};
    return ExecEntry::fromJson(item_data(idx));
}

void ExecControlsBar::clear_executables() {
    exec_combo_->clear();
    // Re-add the sentinel so add_entry() works
    exec_combo_->addItem(tr(kAddNewEntryText), QVariant(QJsonObject()));
}

bool ExecControlsBar::select_executable(const QString& path) {
    if (path.isEmpty()) return false;
    for (int i = 1; i < exec_combo_->count(); ++i) {
        if (item_data(i)["path"].toString() == path) {
            exec_combo_->setCurrentIndex(i);
            return true;
        }
    }
    return false;
}

void ExecControlsBar::set_executables(const QStringList& names, const QString& default_name,
                                       const std::filesystem::path& game_dir,
                                       const std::filesystem::path& icon_cache_dir,
                                       const std::filesystem::path& staging_dir) {
    exec_combo_->clear();
    // Sentinel stays first (index 0); real executables are appended after it
    exec_combo_->addItem(tr(kAddNewEntryText), QVariant(QJsonObject()));

    // Keep the resolution context for later add_entry() calls (dialog accept,
    // missing-exe prune) so every rebuild path resolves icons identically.
    game_dir_ = game_dir;
    icon_cache_dir_ = icon_cache_dir;
    staging_dir_ = staging_dir;

    for (int i = 0; i < names.size(); ++i) {
        auto raw = names[i];

        // Detect JSON string vs plain legacy path
        ExecEntry entry;
        if (raw.startsWith('{')) {
            QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
            if (doc.isObject()) {
                entry = ExecEntry::fromJson(doc.object());
            } else {
                entry = ExecEntry::fromLegacyPath(raw);
            }
        } else {
            entry = ExecEntry::fromLegacyPath(raw);
        }

        auto display = exec_entry_display_name(entry);
        exec_combo_->addItem(resolveEntryIcon(entry, game_dir_, icon_cache_dir_, staging_dir_), display, QVariant(entry.toJson()));
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
