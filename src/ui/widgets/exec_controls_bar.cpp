#include "ui/widgets/exec_controls_bar.h"
#include "engine/log/logger.h"

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

QIcon extractExeIcon(const QString& exePath, const std::filesystem::path& icon_cache_dir) {
    auto& log = engine::Logger::instance();
    auto exe_std = exePath.toStdString();
    auto exe_file = QFileInfo(exePath).fileName();
    auto cache_key = exe_file + ".ico";
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
                    auto stderr_out = proc.readAllStandardError().toStdString();
                    log.warn("wrestool failed for " + exe_std + " (exit " +
                             std::to_string(proc.exitCode()) + "): " + stderr_out);
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

    QFileIconProvider provider;
    return provider.icon(QFileInfo(exePath));
}

// Resolve the display text for an ExecEntry (title, or filename from path, or "Untitled")
QString displayTextForEntry(const QJsonObject& obj) {
    auto title = obj["title"].toString();
    if (!title.isEmpty()) return title;
    auto path = obj["path"].toString();
    if (!path.isEmpty()) {
        auto last_slash = path.lastIndexOf('/');
        return last_slash >= 0 ? path.mid(last_slash + 1) : path;
    }
    return "Untitled";
}

}  // namespace

namespace ui {

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

    // When "Add new entry..." is chosen, emit signal and restore previous selection
    connect(exec_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && exec_combo_->itemData(index).toJsonObject().isEmpty()
            && exec_combo_->count() > 1) {
            int prev = index > 0 ? index - 1 : 1;
            QSignalBlocker blocker(exec_combo_);
            exec_combo_->setCurrentIndex(prev);
            emit add_entry_requested();
        }
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
    for (int i = 0; i < exec_combo_->count() - 1; ++i) {
        auto obj = item_data(i);
        auto p = obj["path"].toString();
        if (!p.isEmpty())
            paths.append(p);
    }
    return paths;
}

QVector<ExecEntry> ExecControlsBar::executable_entries() const {
    QVector<ExecEntry> entries;
    for (int i = 0; i < exec_combo_->count() - 1; ++i) {
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
    int insert_pos = exec_combo_->count() - 1;
    exec_combo_->insertItem(insert_pos, icon, displayTextForEntry(e.toJson()), QVariant(e.toJson()));
    exec_combo_->setCurrentIndex(insert_pos);
}

void ExecControlsBar::add_entry(const ExecEntry& entry) {
    QIcon icon;

    // Custom icon path takes priority
    if (!entry.icon_path.isEmpty()) {
        QPixmap pix(entry.icon_path);
        if (!pix.isNull())
            icon = QIcon(pix);
    }

    // Fall back to filesystem icon from the binary path
    if (icon.isNull() && !entry.path.isEmpty()) {
        QFileIconProvider provider;
        icon = provider.icon(QFileInfo(entry.path));
    }

    int insert_pos = exec_combo_->count() - 1;
    exec_combo_->insertItem(insert_pos, icon, displayTextForEntry(entry.toJson()), QVariant(entry.toJson()));
    exec_combo_->setCurrentIndex(insert_pos);
}

ExecEntry ExecControlsBar::current_entry() const {
    int idx = exec_combo_->currentIndex();
    if (idx < 0 || idx >= exec_combo_->count() - 1)
        return {};
    return ExecEntry::fromJson(item_data(idx));
}

void ExecControlsBar::clear_executables() {
    exec_combo_->clear();
    // Re-add the sentinel so add_entry() works
    exec_combo_->addItem(tr(kAddNewEntryText), QVariant(QJsonObject()));
}

void ExecControlsBar::set_executables(const QStringList& names, const QString& default_name,
                                       const std::filesystem::path& game_dir,
                                       const std::filesystem::path& icon_cache_dir) {
    exec_combo_->clear();

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

        auto toml_path = entry.path;  // relative path in toml
        auto display = displayTextForEntry(entry.toJson());

        // Try icon
        if (!game_dir.empty()) {
            auto full_path = game_dir / toml_path.toStdString();
            if (std::filesystem::exists(full_path)) {
                auto qpath = QString::fromStdString(full_path.string());
                exec_combo_->addItem(extractExeIcon(qpath, icon_cache_dir), display, QVariant(entry.toJson()));
                continue;
            }
        }
        exec_combo_->addItem(display, QVariant(entry.toJson()));
    }

    exec_combo_->addItem(tr(kAddNewEntryText), QVariant(QJsonObject()));

    if (!default_name.isEmpty()) {
        for (int i = 0; i < exec_combo_->count(); ++i) {
            auto p = item_data(i)["path"].toString();
            if (!p.isEmpty() && p == default_name) {
                exec_combo_->setCurrentIndex(i);
                return;
            }
        }
    }
}

}  // namespace ui
