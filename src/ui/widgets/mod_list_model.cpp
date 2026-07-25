#include "ui/widgets/mod_list_model.h"

#include <QColor>

namespace ui {

ModListModel::ModListModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int ModListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : mods_.size();
}

int ModListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ModListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= mods_.size()) return {};

    const auto& mod = mods_[index.row()];

    if (role == Qt::CheckStateRole && index.column() == Enabled) {
        return mod.enabled ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case Name: return mod.name;
            case Version: return mod.version;
            case Status: {
                if (mod.conflicts.isEmpty()) return "OK";
                return QString("%1 conflict(s)").arg(mod.conflicts.size());
            }
            case Priority: return mod.priority;
        }
    }
    if (role == Qt::ForegroundRole && index.column() == Status) {
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
        case Status: return "Status";
        case Priority: return "Priority";
    }
    return {};
}

Qt::ItemFlags ModListModel::flags(const QModelIndex& index) const {
    auto f = QAbstractTableModel::flags(index);
    if (index.column() == Enabled) {
        f |= Qt::ItemIsUserCheckable;
    }
    f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    return f;
}

Qt::DropActions ModListModel::supportedDropActions() const {
    return Qt::MoveAction;
}

bool ModListModel::moveRows(const QModelIndex& srcParent, int srcRow, int count,
                            const QModelIndex& dstParent, int dstRow) {
    if (srcParent.isValid() || dstParent.isValid()) return false;
    if (srcRow < 0 || srcRow + count > mods_.size()) return false;
    if (dstRow < 0 || dstRow > mods_.size()) return false;
    if (count != 1) return false;

    int dest = dstRow > srcRow ? dstRow - 1 : dstRow;

    beginMoveRows(srcParent, srcRow, srcRow, dstParent, dstRow);
    auto item = mods_.takeAt(srcRow);
    mods_.insert(dest, std::move(item));
    endMoveRows();

    renumber_priorities();
    emit mod_list_changed();
    return true;
}

void ModListModel::add_mod(const QString& id, const QString& name, const QString& version) {
    beginInsertRows({}, mods_.size(), mods_.size());
    ModEntry entry;
    entry.id = id;
    entry.name = name;
    entry.version = version;
    entry.enabled = true;
    entry.priority = mods_.size();
    mods_.append(entry);
    endInsertRows();
    emit mod_list_changed();
}

void ModListModel::remove_mod(const QString& id) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
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
            emit dataChanged(index(i, Status), index(i, Status));
            return;
        }
    }
}

void ModListModel::renumber_priorities() {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].priority != i) {
            mods_[i].priority = i;
            emit dataChanged(index(i, Priority), index(i, Priority));
        }
    }
}

QStringList ModListModel::enabled_mod_ids() const {
    QStringList ids;
    for (const auto& m : mods_) {
        if (m.enabled) ids.append(m.id);
    }
    return ids;
}

int ModListModel::priority_of(const QString& id) const {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) return i;
    }
    return -1;
}

}  // namespace ui
