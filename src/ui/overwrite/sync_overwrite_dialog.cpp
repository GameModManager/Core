#include "ui/overwrite/sync_overwrite_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTreeWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

SyncOverwriteDialog::SyncOverwriteDialog(const Context& ctx, QWidget* parent)
    : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("Sync to Mods"));
    setMinimumSize(600, 460);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    auto* hint = new QLabel(this);
    layout->addWidget(hint);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("File"), tr("Sync to")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(tree_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SyncOverwriteDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    files_ = engine::collect_overwrite_sync_files(
        ctx.overwrite_dir, ctx.mods_dir, ctx.mod_infos, ctx.mods_subpath,
        ctx.conflict_reversed, ctx.include_mod_id, ctx.game_dir);

    if (files_.empty()) {
        hint->setText(tr("The Overwrite folder is empty."));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    hint->setText(
        tr("Choose where each Overwrite file should be moved to. A file with no "
           "owner stays in Overwrite."));
    build_tree();
    tree_->expandAll();
}

void SyncOverwriteDialog::build_tree() {
    for (size_t i = 0; i < files_.size(); ++i) {
        const auto& rel = files_[i].overwrite_rel;
        DirNode* node = &root_;
        std::string segment;
        for (size_t pos = 0; pos <= rel.size(); ++pos) {
            if (pos == rel.size() || rel[pos] == '/') {
                if (segment.empty()) {
                    segment.clear();
                    continue;
                }
                if (pos == rel.size()) {
                    node->files.push_back({segment, i});
                } else {
                    node = &node->children[segment];
                }
                segment.clear();
            } else {
                segment += rel[pos];
            }
        }
    }

    auto* top = new QTreeWidgetItem(tree_);
    top->setText(0, QString::fromStdString(ctx_.mods_subpath.empty()
                                               ? "<root>"
                                               : ctx_.mods_subpath));
    add_items(top, root_);
    top->setExpanded(true);
}

void SyncOverwriteDialog::add_items(QTreeWidgetItem* parent_item,
                                    const DirNode& node) {
    for (const auto& [name, child] : node.children) {
        auto* dir_item = new QTreeWidgetItem(parent_item);
        dir_item->setText(0, QString::fromStdString(name));
        add_items(dir_item, child);
    }
    for (const auto& leaf : node.files) {
        auto* file_item = new QTreeWidgetItem(parent_item);
        file_item->setText(0, QString::fromStdString(leaf.name));
        add_file_row(file_item, leaf.index);
    }
}

void SyncOverwriteDialog::add_file_row(QTreeWidgetItem* item, size_t file_index) {
    const auto& f = files_[file_index];
    auto* combo = new QComboBox(tree_);
    combo->addItem(tr("<don't sync>"), QString());

    for (const auto& owner : f.owners) {
        combo->addItem(QString::fromStdString(owner.mod_id),
                       QString::fromStdString(owner.mod_id));
    }
    if (f.game_has_file && !ctx_.game_folder.empty()) {
        combo->addItem(QString::fromStdString(
                           ctx_.game_label.empty() ? ctx_.game_folder : ctx_.game_label),
                       QString::fromStdString(ctx_.game_folder));
    }

    if (!f.owners.empty()) {
        combo->setCurrentIndex(1);  // winner first
    } else if (f.game_has_file && !ctx_.game_folder.empty()) {
        combo->setCurrentIndex(combo->count() - 1);  // game-origin
    } else {
        combo->setCurrentIndex(0);  // <don't sync>
    }

    tree_->setItemWidget(item, 1, combo);
    rows_.push_back({combo, file_index});
}

void SyncOverwriteDialog::accept() {
    targets_.clear();
    for (const auto& row : rows_) {
        const auto folder = row.combo->currentData().toString();
        if (folder.isEmpty()) continue;
        targets_.push_back(
            {files_[row.index].overwrite_rel, folder.toStdString()});
    }
    QDialog::accept();
}

std::vector<engine::OverwriteSyncTarget> SyncOverwriteDialog::targets() const {
    return targets_;
}

}  // namespace ui
