#include "ui/widgets/mod_list_model.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QMimeData>
#include <QTreeView>

namespace ui {

ModListModel::ModListModel(QObject* parent)
    : QAbstractTableModel(parent) {
    ensure_overwrite_present();
}

int ModListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : mods_.size();
}

int ModListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ModListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= mods_.size()) return {};

    const auto& mod = mods_[index.row()];

    // --- Separator: colored background spans all columns ---
    if (mod.is_separator) {
        if (role == Qt::BackgroundRole || role == Qt::ToolTipRole) {
            if (role == Qt::BackgroundRole) {
                QColor bg(mod.separator_color.isEmpty() ? "#888888" : mod.separator_color);
                return QBrush(bg);
            }
            return mod.name;
        }
        if (role == Qt::ForegroundRole) {
            QColor bg(mod.separator_color.isEmpty() ? "#888888" : mod.separator_color);
            int l = static_cast<int>(0.299 * bg.red() + 0.587 * bg.green() + 0.114 * bg.blue());
            return QBrush(l > 128 ? QColor(0, 0, 0) : QColor(255, 255, 255));
        }
        if (role == Qt::FontRole && index.column() == Name) {
            QFont f;
            f.setBold(true);
            return f;
        }
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            switch (index.column()) {
                case Enabled: {
                    return mod.folded ? QString("\u25B6") : QString("\u25BC");
                }
                case Name: return mod.name;
                case Version: return QString();
                case Flags: return QString();
                case Priority: return QString();
            }
        }
        if (role == Qt::TextAlignmentRole && index.column() == Enabled) {
            return static_cast<int>(Qt::AlignCenter);
        }
        if (role == Qt::FontRole && index.column() == Enabled) {
            QFont f;
            f.setPointSize(8);
            return f;
        }
        return {};
    }

    // --- Overwrite: italic gray name ---
    if (mod.is_overwrite) {
        if (role == Qt::FontRole && index.column() == Name) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ForegroundRole && index.column() == Name) {
            return QColor(160, 160, 160);
        }
    }

    // --- Regular mod + Overwrite shared ---
    if (role == Qt::CheckStateRole && index.column() == Enabled) {
        return mod.enabled ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::TextAlignmentRole && index.column() == Enabled) {
        return Qt::AlignCenter;
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case Name: return mod.name;
            case Version: return mod.version;
            case Flags: {
                if (mod.conflicts.isEmpty()) return "OK";
                return QString("%1 conflict(s)").arg(mod.conflicts.size());
            }
            case Priority: return mod.priority;
        }
    }
    if (role == Qt::ForegroundRole && index.column() == Flags) {
        if (!mod.conflicts.isEmpty()) return QColor(255, 80, 80);
        return QColor(80, 200, 80);
    }
    if (role == Qt::TextAlignmentRole && index.column() == Priority) {
        return Qt::AlignCenter;
    }
    return {};
}

bool ModListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= mods_.size()) return false;

    if (mods_[index.row()].is_separator || mods_[index.row()].is_overwrite) return false;

    if (role == Qt::CheckStateRole && index.column() == Enabled) {
        mods_[index.row()].enabled = (value.toInt() == Qt::Checked);
        emit dataChanged(index, index, {Qt::CheckStateRole});
        emit mod_list_changed();
        return true;
    }
    return false;
}

QVariant ModListModel::headerData(int section, Qt::Orientation, int role) const {
    if (role != Qt::DisplayRole) return {};
    switch (section) {
        case Enabled: return "";
        case Name: return "Name";
        case Version: return "Version";
        case Flags: return "Flags";
        case Priority: return "Priority";
    }
    return {};
}

Qt::ItemFlags ModListModel::flags(const QModelIndex& index) const {
    auto f = QAbstractTableModel::flags(index);
    if (!index.isValid()) return f;

    const auto& mod = mods_[index.row()];

    if (mod.is_separator) {
        f &= ~Qt::ItemIsUserCheckable;
        f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
        return f;
    }

    if (index.column() == Enabled) {
        f |= Qt::ItemIsUserCheckable;
    }

    if (mod.is_overwrite) {
        f &= ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    } else {
        f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    }
    return f;
}

Qt::DropActions ModListModel::supportedDropActions() const {
    return Qt::MoveAction;
}

Qt::DropActions ModListModel::supportedDragActions() const {
    return Qt::MoveAction;
}

QStringList ModListModel::mimeTypes() const {
    return { kModListMimeType };
}

QMimeData* ModListModel::mimeData(const QModelIndexList& indexes) const {
    auto* mime = new QMimeData;

    QList<int> rows;
    for (const auto& idx : indexes) {
        if (idx.isValid() && !rows.contains(idx.row())) {
            int r = idx.row();
            if (r < mods_.size() && !mods_[r].is_overwrite) {
                rows.append(r);
            }
        }
    }
    std::sort(rows.begin(), rows.end());

    QByteArray encoded;
    for (int i = 0; i < rows.size(); ++i) {
        if (i > 0) encoded += ',';
        encoded += QByteArray::number(rows[i]);
    }

    mime->setData(kModListMimeType, encoded);
    return mime;
}

bool ModListModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
                                int row, int column, const QModelIndex& parent) {
    Q_UNUSED(column);

    // QTreeView passes parent=valid, row=-1 when dropping ON an item.
    // Convert to between-row semantics (like IsaacMM's FlatDropModel).
    if (parent.isValid()) {
        row = parent.row() + 1;
    }

    if (action != Qt::MoveAction) return false;
    if (!data->hasFormat(kModListMimeType)) return false;

    QByteArray encoded = data->data(kModListMimeType);
    QList<int> sourceRows;
    for (const auto& token : encoded.split(',')) {
        if (!token.isEmpty()) {
            bool ok = false;
            int r = token.toInt(&ok);
            if (ok) sourceRows.append(r);
        }
    }

    if (sourceRows.isEmpty()) return false;

    QList<int> validSources;
    for (int r : sourceRows) {
        if (r >= 0 && r < mods_.size() && !mods_[r].is_overwrite) {
            validSources.append(r);
        }
    }
    if (validSources.isEmpty()) return false;

    QList<ModEntry> toMove;
    for (int r : validSources) {
        toMove.append(mods_[r]);
    }

    // Remove source rows (reverse order to keep indices valid)
    for (int i = validSources.size() - 1; i >= 0; --i) {
        beginRemoveRows({}, validSources[i], validSources[i]);
        mods_.removeAt(validSources[i]);
        endRemoveRows();
    }

    // Adjust target row: each source row before the target shifts it down
    int targetRow = row;
    for (int src : validSources) {
        if (src < targetRow) targetRow--;
    }
    if (targetRow < 0) targetRow = 0;
    if (targetRow > mods_.size()) targetRow = mods_.size();

    // Prevent dropping above Overwrite
    if (targetRow <= 0 && !mods_.isEmpty() && mods_.first().is_overwrite) {
        targetRow = 1;
    }

    for (int i = 0; i < toMove.size(); ++i) {
        beginInsertRows({}, targetRow + i, targetRow + i);
        mods_.insert(targetRow + i, toMove[i]);
        endInsertRows();
    }

    renumber_priorities();
    emit mod_list_changed();
    return true;
}

bool ModListModel::moveRows(const QModelIndex& srcParent, int srcRow, int count,
                            const QModelIndex& dstParent, int dstRow) {
    if (srcParent.isValid() || dstParent.isValid()) return false;
    if (srcRow < 0 || srcRow + count > mods_.size()) return false;
    if (dstRow < 0 || dstRow > mods_.size()) return false;
    if (count != 1) return false;

    if (mods_[srcRow].is_overwrite) return false;

    int dest = dstRow > srcRow ? dstRow - 1 : dstRow;
    if (dest <= 0 && mods_.first().is_overwrite) dest = 1;

    beginMoveRows(srcParent, srcRow, srcRow, srcParent, dest + (dest >= srcRow ? 1 : 0));
    auto item = mods_.takeAt(srcRow);
    mods_.insert(dest, std::move(item));
    endMoveRows();

    renumber_priorities();
    emit mod_list_changed();
    return true;
}

void ModListModel::add_mod(const QString& id, const QString& name, const QString& version, int priority) {
    int insert_pos = mods_.size();
    beginInsertRows({}, insert_pos, insert_pos);
    ModEntry entry;
    entry.id = id;
    entry.name = name;
    entry.version = version;
    entry.enabled = true;
    entry.priority = priority >= 0 ? priority : insert_pos;
    mods_.insert(insert_pos, entry);
    endInsertRows();
    if (priority < 0) renumber_priorities();
    emit mod_list_changed();
}

void ModListModel::add_separator(const QString& id, const QString& name, const QString& color) {
    int insert_pos = mods_.size();
    beginInsertRows({}, insert_pos, insert_pos);
    ModEntry entry;
    entry.id = id;
    entry.name = name;
    entry.separator_color = color;
    entry.is_separator = true;
    entry.enabled = true;
    mods_.insert(insert_pos, entry);
    endInsertRows();
    renumber_priorities();
    emit mod_list_changed();
}

void ModListModel::remove_mod(const QString& id) {
    if (id == kOverwriteModId) return;

    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && !mods_[i].is_overwrite) {
            beginRemoveRows({}, i, i);
            mods_.removeAt(i);
            endRemoveRows();
            renumber_priorities();
            emit mod_list_changed();
            return;
        }
    }
}

void ModListModel::toggle_mod(const QString& id) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].enabled = !mods_[i].enabled;
            emit dataChanged(index(i, Enabled), index(i, Enabled), {Qt::CheckStateRole});
            emit mod_list_changed();
            return;
        }
    }
}

void ModListModel::set_conflicts(const QString& id, const QStringList& conflicting_ids) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].conflicts = conflicting_ids;
            emit dataChanged(index(i, Flags), index(i, Flags));
            return;
        }
    }
}

void ModListModel::renumber_priorities() {
    int priority = 0;
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].is_separator) continue;
        if (mods_[i].priority != priority) {
            mods_[i].priority = priority;
            emit dataChanged(index(i, Priority), index(i, Priority));
        }
        ++priority;
    }
}

QStringList ModListModel::enabled_mod_ids() const {
    QStringList ids;
    for (const auto& m : mods_) {
        if (m.enabled && !m.is_separator && !m.is_overwrite) ids.append(m.id);
    }
    return ids;
}

int ModListModel::priority_of(const QString& id) const {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) return i;
    }
    return -1;
}

int ModListModel::overwrite_row() const {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].is_overwrite) return i;
    }
    return -1;
}

bool ModListModel::is_overwrite(int row) const {
    return row >= 0 && row < mods_.size() && mods_[row].is_overwrite;
}

void ModListModel::set_folded(int row, bool folded) {
    if (row < 0 || row >= mods_.size()) return;
    if (!mods_[row].is_separator) return;
    if (mods_[row].folded == folded) return;
    mods_[row].folded = folded;
    emit dataChanged(index(row, Enabled), index(row, Enabled));
    apply_fold_state();
}

void ModListModel::apply_fold_state() {
    auto* tree = qobject_cast<QTreeView*>(mod_view_);
    if (!tree) return;

    bool hiding = false;
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].is_separator) {
            hiding = mods_[i].folded;
            continue;
        }
        if (hiding) {
            if (mods_[i].is_overwrite) {
                hiding = false;
            } else {
                tree->setRowHidden(i, QModelIndex(), true);
                continue;
            }
        }
        tree->setRowHidden(i, QModelIndex(), false);
    }
}

QStringList ModListModel::existing_separator_names() const {
    QStringList names;
    for (const auto& m : mods_) {
        if (m.is_separator) names.append(m.name);
    }
    return names;
}

void ModListModel::reset_with_order(const QVector<ModEntry>& entries) {
    beginResetModel();
    mods_ = entries;
    endResetModel();
    renumber_priorities();
}

void ModListModel::set_conflict_order_reversed(bool reversed) {
    conflict_order_reversed_ = reversed;
}

void ModListModel::ensure_overwrite_present() {
    for (const auto& m : mods_) {
        if (m.is_overwrite) return;
    }
    ModEntry entry;
    entry.id = kOverwriteModId;
    entry.name = kOverwriteModName;
    entry.version = "";
    entry.enabled = true;
    entry.priority = 0;
    entry.is_overwrite = true;
    beginInsertRows({}, 0, 0);
    mods_.insert(0, entry);
    endInsertRows();
}

}  // namespace ui
