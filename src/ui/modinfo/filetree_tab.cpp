#include "ui/modinfo/filetree_tab.h"

#include "engine/core/util/fs_utils.h"
#include "ui/theme/icon_manager.h"
#include "ui/viewer/file_viewer.h"

#include <QDesktopServices>
#include <QDialog>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace ui {

namespace {

class PreviewDialog : public QDialog {
public:
    explicit PreviewDialog(const QString& path, QWidget* parent = nullptr)
        : QDialog(parent), viewer_(new FileViewer(this)) {
        setWindowTitle(QFileInfo(path).fileName());
        resize(700, 500);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(viewer_);
        if (!viewer_->open(path)) {
            // Unsupported preview type — fall back to the default app.
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            reject();
        }
    }

private:
    FileViewer* viewer_ = nullptr;
};

}  // namespace

FiletreeTab::FiletreeTab(QWidget* parent) : ModInfoTab(parent) {
    tree_ = new QTreeView(this);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setUniformRowHeights(true);
    tree_->setHeaderHidden(false);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->setStretchLastSection(false);
    tree_->setColumnWidth(0, 320);

    model_ = new QFileSystemModel(this);
    model_->setReadOnly(true);
    tree_->setModel(model_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);

    connect(tree_, &QTreeView::customContextMenuRequested, this,
            &FiletreeTab::show_menu);
    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex&) {
        on_open();
    });
}

FiletreeTab::~FiletreeTab() = default;

void FiletreeTab::set_mod(const ModInfoData& data) {
    root_path_ = data.mod_dir.absolutePath();
    set_has_data(true);
}

void FiletreeTab::first_activation() {
    if (root_path_.isEmpty()) return;
    model_->setRootPath(root_path_);
    tree_->setRootIndex(model_->index(root_path_));
    tree_->expandToDepth(0);
}

QString FiletreeTab::selected_path() const {
    const QModelIndex index = tree_->currentIndex();
    if (!index.isValid()) return {};
    return model_->filePath(index);
}

void FiletreeTab::show_menu(const QPoint& pos) {
    const QString path = selected_path();
    QMenu menu(this);

    auto* open = menu.addAction(tr("&Open"));
    QObject::connect(open, &QAction::triggered, this, &FiletreeTab::on_open);

    auto* preview = menu.addAction(tr("&Preview"));
    QObject::connect(preview, &QAction::triggered, this, &FiletreeTab::on_preview);

    auto* explore = menu.addAction(tr("Open in &Explorer"));
    QObject::connect(explore, &QAction::triggered, this, &FiletreeTab::on_explore);

    auto* new_folder = menu.addAction(tr("New &Folder"));
    QObject::connect(new_folder, &QAction::triggered, this,
                     &FiletreeTab::on_new_folder);

    menu.addSeparator();

    const bool is_hidden = engine::is_hidden_file(path.toStdString());
    auto* hide = menu.addAction(is_hidden ? tr("&Unhide") : tr("&Hide"));
    QObject::connect(hide, &QAction::triggered, this, &FiletreeTab::on_hide);

    auto* rename = menu.addAction(tr("&Rename..."));
    QObject::connect(rename, &QAction::triggered, this, &FiletreeTab::on_rename);

    auto* remove = menu.addAction(tr("&Delete"));
    remove->setIcon(engine::IconManager::instance().resolve_icon("edit-delete"));
    QObject::connect(remove, &QAction::triggered, this, &FiletreeTab::on_delete);

    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void FiletreeTab::on_open() {
    const QString path = selected_path();
    if (path.isEmpty() || !current().open_file) return;
    current().open_file(path);
}

void FiletreeTab::on_preview() {
    const QString path = selected_path();
    if (path.isEmpty()) return;
    if (QFileInfo(path).isFile()) {
        PreviewDialog dlg(path, this);
        dlg.exec();
    } else {
        on_explore();
    }
}

void FiletreeTab::on_explore() {
    QString path = selected_path();
    if (path.isEmpty()) return;
    if (!QFileInfo(path).isDir()) path = QFileInfo(path).absolutePath();
    if (current().open_explorer) {
        current().open_explorer();
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void FiletreeTab::on_rename() {
    const QString path = selected_path();
    if (path.isEmpty()) return;
    const QFileInfo info(path);

    bool ok = false;
    const QString new_name = QInputDialog::getText(
        this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
        info.fileName(), &ok);
    if (!ok || new_name.trimmed().isEmpty() ||
        new_name.trimmed() == info.fileName()) {
        return;
    }

    const QDir parent = info.dir();
    const QString new_path = parent.filePath(new_name.trimmed());
    if (QFileInfo::exists(new_path)) {
        QMessageBox::warning(this, tr("Rename"),
                             tr("\"%1\" already exists.").arg(new_path));
        return;
    }
    if (!QFile::rename(path, new_path)) {
        QMessageBox::warning(this, tr("Rename"),
                             tr("Failed to rename \"%1\".").arg(path));
    }
}

void FiletreeTab::on_delete() {
    const QString path = selected_path();
    if (path.isEmpty()) return;
    if (QMessageBox::question(
            this, tr("Delete"),
            tr("Delete \"%1\"?").arg(path),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes) {
        return;
    }
    const QFileInfo info(path);
    bool ok = false;
    if (info.isDir()) {
        QDir dir(path);
        ok = dir.removeRecursively();
    } else {
        ok = QFile::remove(path);
    }
    if (!ok) {
        QMessageBox::warning(this, tr("Delete"),
                             tr("Failed to delete \"%1\".").arg(path));
    }
}

void FiletreeTab::on_hide() {
    const QString path = selected_path();
    if (path.isEmpty() || !current().hide_file) return;
    const bool hidden = engine::is_hidden_file(path.toStdString());
    current().hide_file(path, !hidden);
}

void FiletreeTab::on_new_folder() {
    const QModelIndex index = tree_->currentIndex();
    QString parent_path =
        index.isValid() && model_->isDir(index) ? model_->filePath(index)
                                                : model_->rootPath();
    if (parent_path.isEmpty()) parent_path = model_->rootPath();

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New Folder"), tr("Folder name:"), QLineEdit::Normal,
        QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QDir parent(parent_path);
    const QString new_path = parent.filePath(name.trimmed());
    if (QFileInfo::exists(new_path)) {
        QMessageBox::warning(this, tr("New Folder"),
                             tr("\"%1\" already exists.").arg(new_path));
        return;
    }
    if (!parent.mkdir(name.trimmed())) {
        QMessageBox::warning(this, tr("New Folder"),
                             tr("Failed to create \"%1\".").arg(new_path));
    }
}

}  // namespace ui
