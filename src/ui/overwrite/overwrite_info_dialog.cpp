#include "ui/overwrite/overwrite_info_dialog.h"

#include "engine/util/fs_utils.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QShortcut>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>

namespace ui {

OverwriteInfoDialog::OverwriteInfoDialog(
    const std::filesystem::path& overwrite_dir,
    const std::string& mods_subpath,
    QWidget* parent)
    : QDialog(parent), overwrite_dir_(overwrite_dir), mods_subpath_(mods_subpath) {
    setWindowTitle(tr("Overwrite"));
    setWindowModality(Qt::NonModal);
    resize(560, 420);

    model_ = new QFileSystemModel(this);
    model_->setReadOnly(false);

    view_ = new QTreeView(this);
    view_->setModel(model_);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setUniformRowHeights(true);
    view_->setColumnWidth(0, 250);
    // Drag files/folders onto a mod row in the main window to move them into
    // that mod (MO2's drop-to-mod from the overwrite info dialog).
    view_->setDragEnabled(true);
    view_->setDragDropMode(QAbstractItemView::DragOnly);
    view_->setDragDropOverwriteMode(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);

    new QShortcut(QKeySequence::Delete, this,
                  [this]() { delete_selected(); });

    connect(view_, &QTreeView::customContextMenuRequested,
            this, &OverwriteInfoDialog::show_context_menu);
    connect(view_, &QTreeView::doubleClicked,
            this, [this](const QModelIndex& index) {
                const auto path = model_->filePath(index);
                if (model_->isDir(index)) return;
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });

    set_path(overwrite_dir);
}

void OverwriteInfoDialog::set_path(const std::filesystem::path& overwrite_dir) {
    if (overwrite_dir.empty()) return;
    overwrite_dir_ = overwrite_dir;
    const auto root = model_->setRootPath(QString::fromStdString(overwrite_dir.string()));
    view_->setRootIndex(root);
    // Default sort by name for a stable layout.
    view_->header()->setSortIndicator(0, Qt::AscendingOrder);
    view_->sortByColumn(0, Qt::AscendingOrder);
}

std::vector<QModelIndex> OverwriteInfoDialog::selected_indexes() const {
    std::vector<QModelIndex> result;
    const auto rows = view_->selectionModel()->selectedRows(0);
    for (const auto& index : rows) {
        if (index.isValid()) result.push_back(index);
    }
    return result;
}

bool OverwriteInfoDialog::is_mapping_root(const QModelIndex& index) const {
    // A top-level directory whose name equals the first segment of the
    // mod-mapping subpath (e.g. "Data"). Case-insensitive, matching the
    // engine's overwrite_utils.
    const auto path = model_->filePath(index);
    std::error_code ec;
    if (!std::filesystem::is_directory(path.toStdString(), ec) || ec) return false;
    if (path.toStdString() == overwrite_dir_.string()) return true;

    std::string normalized = mods_subpath_;
    auto first_segment = normalized.substr(0, normalized.find('/'));
    auto name = std::filesystem::path(path.toStdString()).filename().string();
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    return lower(name) == lower(first_segment);
}

void OverwriteInfoDialog::show_context_menu(const QPoint& pos) {
    QMenu menu(this);
    auto* open_act = menu.addAction(tr("&Open"), this, [this]() { open_selected(); });
    auto* rename_act = menu.addAction(tr("&Rename"), this, [this]() { rename_selected(); });
    auto* new_folder_act =
        menu.addAction(tr("&New Folder"), this, [this]() { new_folder(); });
    menu.addSeparator();
    auto* delete_act = menu.addAction(tr("&Delete"), this, [this]() { delete_selected(); });

    const auto sel = selected_indexes();
    const bool single = sel.size() == 1;
    open_act->setEnabled(single || sel.size() > 1);
    rename_act->setEnabled(single && !model_->isDir(sel[0]));
    new_folder_act->setEnabled(true);
    delete_act->setEnabled(!sel.empty());

    if (sel.size() == 1) {
        const auto path = model_->filePath(sel[0]);
        open_act->setEnabled(std::filesystem::exists(path.toStdString()));
        rename_act->setEnabled(!is_mapping_root(sel[0]));
    }

    menu.exec(view_->viewport()->mapToGlobal(pos));
}

void OverwriteInfoDialog::delete_selected() {
    auto sel = selected_indexes();
    if (sel.empty()) return;

    // Protect mapping-root directories (e.g. "Data").
    std::vector<QModelIndex> to_delete;
    for (const auto& index : sel) {
        if (is_mapping_root(index)) {
            QMessageBox::warning(
                this, tr("Overwrite"),
                tr("The directory \"%1\" is protected - it maps onto the game's "
                   "mod folder and cannot be deleted.")
                    .arg(model_->fileName(index)));
            continue;
        }
        to_delete.push_back(index);
    }
    if (to_delete.empty()) return;

    const auto reply =
        QMessageBox::question(this, tr("Confirm"),
                              to_delete.size() == 1
                                  ? tr("Are you sure you want to delete \"%1\"? "
                                       "Deleted files go to the system trash.")
                                        .arg(model_->fileName(to_delete[0]))
                                  : tr("Are you sure you want to delete the "
                                       "%1 selected entries? Deleted files go to "
                                       "the system trash.")
                                        .arg(to_delete.size()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    for (const auto& index : to_delete) {
        const auto path = model_->filePath(index).toStdString();
        if (!engine::remove_path(path)) {
            QMessageBox::warning(this, tr("Overwrite"),
                                 tr("Failed to delete \"%1\".")
                                     .arg(model_->fileName(index)));
        }
    }
}

void OverwriteInfoDialog::rename_selected() {
    auto sel = selected_indexes();
    if (sel.size() != 1 || model_->isDir(sel[0])) return;

    bool ok = false;
    const auto new_name = QInputDialog::getText(
        this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
        model_->fileName(sel[0]), &ok);
    if (!ok || new_name.isEmpty()) return;
    if (new_name.contains('/') || new_name.contains('\\')) {
        QMessageBox::warning(this, tr("Rename"), tr("Invalid file name."));
        return;
    }
    model_->setData(sel[0], new_name, Qt::EditRole);
}

void OverwriteInfoDialog::open_selected() {
    for (const auto& index : selected_indexes()) {
        if (model_->isDir(index)) continue;
        const auto path = model_->filePath(index);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void OverwriteInfoDialog::new_folder() {
    auto sel = selected_indexes();
    QModelIndex parent = view_->rootIndex();
    if (sel.size() == 1 && model_->isDir(sel[0])) parent = sel[0];

    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("New Folder"), tr("Name:"),
                                            QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;
    model_->mkdir(parent, name);
}

}  // namespace ui
