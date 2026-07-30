#include "ui/widgets/exec_controls_bar.h"
#include "engine/log/logger.h"

#include <QApplication>
#include <QComboBox>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGridLayout>
#include <QMenu>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QToolButton>

namespace {

// Find wrestool: check bundled tools/ first, then fall back to system PATH
QString findWrestool() {
    // Bundled: <app_dir>/tools/linux/wrestool
    auto app_dir = QCoreApplication::applicationDirPath();
    auto bundled = app_dir + "/../tools/linux/wrestool";
    if (QFileInfo::exists(bundled)) return bundled;

    // System PATH
    auto system = QStandardPaths::findExecutable("wrestool");
    if (!system.isEmpty()) return system;

    return {};
}

QIcon extractExeIcon(const QString& exePath, const std::filesystem::path& icon_cache_dir) {
    auto& log = engine::Logger::instance();
    auto exe_std = exePath.toStdString();

    // Build cache key from executable filename (e.g. "isaac-ng.exe.ico")
    auto exe_file = QFileInfo(exePath).fileName();
    auto cache_key = exe_file + ".ico";
    auto cache_path = std::filesystem::path(icon_cache_dir.string()) / cache_key.toStdString();

    // 1. Check cache
    if (!icon_cache_dir.empty() && std::filesystem::exists(cache_path)) {
        QIcon cached(QString::fromStdString(cache_path.string()));
        if (!cached.isNull()) {
            return cached;
        }
        log.debug("Icon cache file exists but failed to load, re-extracting: " + cache_path.string());
    }

    // 2. Try wrestool to extract the real PE icon (only works on .exe files)
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
                        // Save to cache
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

    // 3. Fallback: QFileIconProvider (system association)
    QFileIconProvider provider;
    return provider.icon(QFileInfo(exePath));
}

}  // namespace

namespace ui {

ExecControlsBar::ExecControlsBar(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Big combo dropdown — spans 2 rows
    exec_combo_ = new QComboBox(this);
    exec_combo_->setMinimumHeight(50);
    exec_combo_->setMinimumWidth(200);
    exec_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    exec_combo_->addItem("Select executable...");
    layout->addWidget(exec_combo_, 0, 0, 2, 1);

    // Run button — top right, same width as shortcut
    run_btn_ = new QToolButton(this);
    run_btn_->setText("Run");
    run_btn_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    run_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    run_btn_->setMinimumHeight(24);
    run_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(run_btn_, 0, 1);

    // Shortcut button with dropdown — below Run
    shortcut_btn_ = new QToolButton(this);
    shortcut_btn_->setText("Shortcut");
    shortcut_btn_->setIcon(style()->standardIcon(QStyle::SP_FileLinkIcon));
    shortcut_btn_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    shortcut_btn_->setMinimumHeight(24);
    shortcut_btn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    shortcut_btn_->setPopupMode(QToolButton::MenuButtonPopup);

    auto* shortcut_menu = new QMenu(this);
    shortcut_menu->addAction("Shortcut to Toolbar");
    shortcut_menu->addAction("Shortcut to Desktop");
    connect(shortcut_menu->actions()[0], &QAction::triggered,
            this, &ExecControlsBar::shortcut_to_toolbar);
    connect(shortcut_menu->actions()[1], &QAction::triggered,
            this, &ExecControlsBar::shortcut_to_desktop);
    shortcut_btn_->setMenu(shortcut_menu);
    layout->addWidget(shortcut_btn_, 1, 1);

    // Combo takes 70%, buttons take 30%
    layout->setColumnStretch(0, 7);
    layout->setColumnStretch(1, 3);

    connect(run_btn_, &QToolButton::clicked, this, &ExecControlsBar::run_clicked);
    // Default shortcut click = toolbar
    connect(shortcut_btn_, &QToolButton::clicked,
            this, &ExecControlsBar::shortcut_to_toolbar);

    // When "Select an executable..." is chosen, emit signal and restore previous selection
    connect(exec_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && exec_combo_->itemData(index).toString().isEmpty()
            && exec_combo_->count() > 1) {
            // Restore previous selection
            int prev = index > 0 ? index - 1 : 1;
            QSignalBlocker blocker(exec_combo_);
            exec_combo_->setCurrentIndex(prev);
            emit select_executable_requested();
        }
    });
}

QString ExecControlsBar::current_executable() const {
    return exec_combo_->currentData().toString();
}

int ExecControlsBar::current_executable_index() const {
    return exec_combo_->currentIndex();
}

QStringList ExecControlsBar::executable_paths() const {
    QStringList paths;
    for (int i = 0; i < exec_combo_->count() - 1; ++i) {
        auto rel = exec_combo_->itemData(i).toString();
        if (!rel.isEmpty())
            paths.append(rel);
    }
    return paths;
}

void ExecControlsBar::add_executable(const QString& display_name, const QString& rel_path,
                                     const QIcon& icon) {
    // Insert before the "Select an executable..." entry (always last)
    int insert_pos = exec_combo_->count() - 1;
    exec_combo_->insertItem(insert_pos, icon, display_name, rel_path);
    exec_combo_->setCurrentIndex(insert_pos);
}

void ExecControlsBar::clear_executables() {
    exec_combo_->clear();
}

void ExecControlsBar::set_executables(const QStringList& names, const QString& default_name,
                                     const std::filesystem::path& game_dir,
                                     const std::filesystem::path& icon_cache_dir) {
    exec_combo_->clear();

    for (int i = 0; i < names.size(); ++i) {
        auto name = names[i];
        auto display = name;
        auto last_slash = name.lastIndexOf('/');
        if (last_slash >= 0) display = name.mid(last_slash + 1);

        // Try to extract the real icon from the .exe via wrestool, fallback to QFileIconProvider
        if (!game_dir.empty()) {
            auto full_path = game_dir / name.toStdString();
            if (std::filesystem::exists(full_path)) {
                auto qpath = QString::fromStdString(full_path.string());
                exec_combo_->addItem(extractExeIcon(qpath, icon_cache_dir), display, name);
                continue;
            }
        }
        exec_combo_->addItem(display, name);
    }
    // Always add "Select an executable..." at the end (empty data = file picker)
    exec_combo_->addItem("Select an executable...", QVariant(""));

    // Select the default
    if (!default_name.isEmpty()) {
        for (int i = 0; i < exec_combo_->count(); ++i) {
            if (exec_combo_->itemData(i).toString() == default_name) {
                exec_combo_->setCurrentIndex(i);
                return;
            }
        }
    }
}

}  // namespace ui
