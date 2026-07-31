#include "ui/widgets/mod_list_model.h"

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QTreeView>

namespace ui {

ModListModel::ModListModel(QObject* parent)
    : QAbstractTableModel(parent) {
    ensure_overwrite_present();
    ensure_merged_present();

    // Load conflict-status icons from <appDir>/../resources/icons/
    auto iconDir = QCoreApplication::applicationDirPath() + "/../resources/icons/";
    overwrite_icon_    = QIcon(iconDir + "conflict-overwrite.png");
    overwritten_icon_  = QIcon(iconDir + "conflict-overwritten.png");
    mixed_icon_        = QIcon(iconDir + "conflict-mixed.png");
    redundant_icon_    = QIcon(iconDir + "conflict-redundant.png");
    hidden_icon_       = QIcon(iconDir + "conflict-hidden.png");
}

// Lay the primary icon out with a secondary icon (e.g. hidden-files badge) to
// its right, since a cell can only carry one DecorationRole icon.
static QIcon stacked_icons(const QIcon& left, const QIcon& right) {
    QSize s = left.actualSize(QSize(16, 16));
    QPixmap pix(s.width() + 2 + s.width(), s.height());
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    left.paint(&p, 0, 0, s.width(), s.height());
    right.paint(&p, s.width() + 2, 0, s.width(), s.height());
    p.end();
    return QIcon(pix);
}

int ModListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : mods_.size();
}

int ModListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ModListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= mods_.size()) return {};

    // ── DecorationRole for Flags column ──────────────────────────────
    if (role == Qt::DecorationRole && index.column() == Flags) {
        const auto& m = mods_[index.row()];
        int row = index.row();
        if (m.is_separator) {
            auto flag = compute_separator_flags(row);
            if (flag == "+") return overwrite_icon_;
            if (flag == "-") return overwritten_icon_;
            if (flag == QString("\u00B1")) return mixed_icon_;
            return {};
        }
        if (!m.tags.isEmpty()) return {};

        QIcon primary;
        if (m.redundant) {
            primary = redundant_icon_;
        } else if (m.conflict_wins > 0 && m.conflict_losses > 0) {
            primary = mixed_icon_;
        } else if (m.conflict_wins > 0) {
            primary = overwrite_icon_;
        } else if (m.conflict_losses > 0) {
            primary = overwritten_icon_;
        }

        if (!m.has_hidden_files) return primary;
        if (primary.isNull()) return hidden_icon_;
        return stacked_icons(primary, hidden_icon_);
    }

    const auto& mod = mods_[index.row()];

    // --- Separator: colored background spans all columns ---
    if (mod.is_separator) {
        if (role == Qt::BackgroundRole) {
            // Conflict highlight takes precedence if this separator is referenced
            if (!selected_mod_id_.isEmpty() && conflict_pairs_.contains(selected_mod_id_)) {
                const auto& pairs = conflict_pairs_[selected_mod_id_];
                if (pairs.wins_against.contains(mod.id))
                    return QBrush(QColor(0, 200, 0, 76));   // 30% green
                if (pairs.loses_to.contains(mod.id))
                    return QBrush(QColor(200, 0, 0, 76));   // 30% red
            }
            QColor bg(mod.separator_color.isEmpty() ? "#888888" : mod.separator_color);
            return QBrush(bg);
        }
        if (role == Qt::ToolTipRole) {
            return mod.name;
        }
        if (role == Qt::ForegroundRole) {
            // Use conflict colors for the Flags column on separators
            if (index.column() == Flags) {
                auto flag = compute_separator_flags(index.row());
                if (flag == "+") return QColor(80, 200, 80);
                if (flag == "-") return QColor(255, 80, 80);
                if (flag == QString("\u00B1")) return QColor(255, 180, 0);
            }
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
                case Flags: return QString();  // shown via DecorationRole icon
                case Priority: return mod.priority;
            }
        }
        if (role == Qt::TextAlignmentRole && index.column() == Enabled) {
            return static_cast<int>(Qt::AlignCenter);
        }
        if (role == Qt::TextAlignmentRole && index.column() == Priority) {
            return static_cast<int>(Qt::AlignCenter);
        }
        if (role == Qt::FontRole && index.column() == Enabled) {
            QFont f;
            f.setPointSize(8);
            return f;
        }
        return {};
    }

    // --- Game-native: italic gray name with "Unmanaged: " prefix ---
    if (mod.is_game_native) {
        if (role == Qt::FontRole && index.column() == Name) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ForegroundRole && index.column() == Name) {
            return QColor(140, 140, 140);
        }
        if (role == Qt::DisplayRole && index.column() == Name) {
            return tr("Unmanaged: %1").arg(mod.name);
        }
    }

    // --- Overwrite: italic gray name ---
    if (mod.is_overwrite) {
        if (role == Qt::FontRole && index.column() == Name) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ForegroundRole && index.column() == Name) {
            if (!overwrite_path_.isEmpty()) {
                QDir dir(overwrite_path_);
                if (dir.exists() && !dir.isEmpty())
                    return QColor(220, 50, 50);  // red = has captured files
            }
            return QColor(160, 160, 160);  // gray = empty
        }
    }

    // --- MERGED: italic blue name ---
    if (mod.is_merged) {
        if (role == Qt::FontRole && index.column() == Name) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ForegroundRole && index.column() == Name) {
            return QColor(80, 180, 255);  // blue
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
                if (!mod.tags.isEmpty()) {
                    return mod.tags.first().type.toUpper();
                }
                // Icons handle conflict states - clear text
                return QString();
            }
            case Priority: return mod.priority;
        }
    }
    if (role == Qt::ForegroundRole && index.column() == Flags) {
        if (!mod.tags.isEmpty()) {
            const auto& firstTag = mod.tags.first();
            if (firstTag.type == "deprecated" || firstTag.type == "incompatible" || firstTag.type == "dirty") {
                return QColor(255, 80, 80);  // Red
            } else if (firstTag.type == "warning") {
                return QColor(255, 180, 0);  // Orange/Yellow
            } else if (firstTag.type == "note") {
                return QColor(80, 180, 255);  // Blue
            } else if (firstTag.type == "clean") {
                return QColor(80, 200, 80);  // Green
            }
        }
        if (mod.conflict_wins > 0 && mod.conflict_losses > 0)
            return QColor(255, 180, 0);  // Orange - mixed
        if (mod.conflict_wins > 0)
            return QColor(80, 200, 80);  // Green - wins
        if (mod.conflict_losses > 0)
            return QColor(255, 80, 80);  // Red - loses
        return QColor(160, 160, 160);  // Gray - no conflicts
    }
    if (role == Qt::TextAlignmentRole && index.column() == Priority) {
        return Qt::AlignCenter;
    }

    // Conflict highlight background (mod + overwrite)
    if (role == Qt::BackgroundRole && !mod.is_separator) {
        if (!selected_mod_id_.isEmpty() && conflict_pairs_.contains(selected_mod_id_)) {
            const auto& pairs = conflict_pairs_[selected_mod_id_];
            if (pairs.wins_against.contains(mod.id))
                return QBrush(QColor(0, 200, 0, 76));
            if (pairs.loses_to.contains(mod.id))
                return QBrush(QColor(200, 0, 0, 76));
        }
    }

    // Subtle background tint for overwrite row (visual separator)
    if (role == Qt::BackgroundRole && mod.is_overwrite)
        return QBrush(QColor(80, 80, 80, 20));

    return {};
}

bool ModListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() >= mods_.size()) return false;

    auto& m = mods_[index.row()];
    if (m.is_separator || m.is_overwrite || m.is_merged || m.is_game_native) return false;

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
        case Name: return tr("Name");
        case Version: return tr("Version");
        case Flags: return tr("Flags");
        case Priority: return tr("Priority");
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
        if (!mod.is_overwrite && !mod.is_merged)
            f |= Qt::ItemIsUserCheckable;
    }

    if (mod.is_overwrite || mod.is_merged || mod.is_game_native) {
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
            if (r < mods_.size() && !mods_[r].is_overwrite && !mods_[r].is_merged) {
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
        if (r >= 0 && r < mods_.size() && !mods_[r].is_overwrite && !mods_[r].is_merged) {
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

    // Prevent dropping onto or past Overwrite (always at bottom)
    int ow_row = overwrite_row();
    if (ow_row >= 0 && targetRow > ow_row)
        targetRow = ow_row;  // drop before Overwrite

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

    if (mods_[srcRow].is_overwrite || mods_[srcRow].is_merged) return false;

    int dest = dstRow > srcRow ? dstRow - 1 : dstRow;
    // Prevent moving onto or past Overwrite or MERGED (always pinned)
    int ow_row = overwrite_row();
    if (ow_row >= 0 && dest >= ow_row)
        dest = ow_row - 1;
    int mg_row = merged_row();
    if (mg_row >= 0 && dest >= mg_row)
        dest = mg_row - 1;
    if (dest < 0) dest = 0;

    beginMoveRows(srcParent, srcRow, srcRow, srcParent, dest + (dest >= srcRow ? 1 : 0));
    auto item = mods_.takeAt(srcRow);
    mods_.insert(dest, std::move(item));
    endMoveRows();

    renumber_priorities();
    emit mod_list_changed();
    return true;
}

void ModListModel::add_mod(const QString& id, const QString& name, const QString& version, int priority, bool is_game_native) {
    int insert_pos = mods_.size();
    beginInsertRows({}, insert_pos, insert_pos);
    ModEntry entry;
    entry.id = id;
    entry.name = name;
    entry.version = version;
    entry.enabled = true;
    entry.priority = priority >= 0 ? priority : insert_pos;
    entry.is_game_native = is_game_native;
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
    if (id == kOverwriteModId || id == kMergedModId) return;

    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && !mods_[i].is_overwrite && !mods_[i].is_merged && !mods_[i].is_game_native) {
            beginRemoveRows({}, i, i);
            mods_.removeAt(i);
            endRemoveRows();
            renumber_priorities();
            emit mod_list_changed();
            return;
        }
    }
}

void ModListModel::remove_all_mods() {
    // Remove everything except Overwrite (including game-native mods)
    bool changed = false;
    for (int i = mods_.size() - 1; i >= 0; --i) {
        if (!mods_[i].is_overwrite && !mods_[i].is_merged) {
            beginRemoveRows({}, i, i);
            mods_.removeAt(i);
            endRemoveRows();
            changed = true;
        }
    }
    if (changed) {
        renumber_priorities();
        emit mod_list_changed();
    }
}

void ModListModel::move_mod(const QString& id, int new_row) {
    int src = -1;
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            src = i;
            break;
        }
    }
    if (src < 0 || src == new_row) return;

    int ow_row = overwrite_row();
    int mg_row = merged_row();
    // Clamp to just before Overwrite/MERGED - after takeAt(src) they shift left by 1
    if (mg_row >= 0 && new_row >= mg_row)
        new_row = mg_row - 1;
    if (ow_row >= 0 && new_row >= ow_row)
        new_row = ow_row - 1;
    if (new_row < 0) new_row = 0;

    beginMoveRows({}, src, src, {}, new_row + (new_row >= src ? 1 : 0));
    auto item = mods_.takeAt(src);
    mods_.insert(new_row, std::move(item));
    endMoveRows();

    renumber_priorities();
    emit mod_list_changed();
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

void ModListModel::set_conflict_stats(const QString& id, int wins, int losses) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].conflict_wins = wins;
            mods_[i].conflict_losses = losses;
            emit dataChanged(index(i, Flags), index(i, Flags),
                             {Qt::DecorationRole, Qt::DisplayRole, Qt::ToolTipRole, Qt::ForegroundRole});
            return;
        }
    }
}

void ModListModel::set_conflict_redundant(const QString& id, bool redundant) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].redundant != redundant) {
            mods_[i].redundant = redundant;
            emit dataChanged(index(i, Flags), index(i, Flags), {Qt::DecorationRole});
            return;
        }
    }
}

void ModListModel::set_hidden_files(const QString& id, bool has_hidden) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].has_hidden_files != has_hidden) {
            mods_[i].has_hidden_files = has_hidden;
            emit dataChanged(index(i, Flags), index(i, Flags), {Qt::DecorationRole});
            return;
        }
    }
}

void ModListModel::set_tags(const QString& id, const QVector<ModTag>& tags) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].tags = tags;
            emit dataChanged(index(i, Flags), index(i, Flags), {Qt::DisplayRole, Qt::ToolTipRole, Qt::ForegroundRole, Qt::DecorationRole});
            return;
        }
    }
}

void ModListModel::set_source_info(const QString& id, const QString& source_type, const QString& source_id) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].source_type = source_type;
            mods_[i].source_id = source_id;
            return;
        }
    }
}

void ModListModel::set_separator_id(const QString& id, const QString& separator_id) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].separator_id = separator_id;
            return;
        }
    }
}

void ModListModel::set_priority(const QString& id, int priority) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            if (mods_[i].priority != priority) {
                mods_[i].priority = priority;
                emit dataChanged(index(i, Priority), index(i, Priority));
            }
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
        if (m.enabled && !m.is_separator && !m.is_overwrite && !m.is_merged) ids.append(m.id);
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
    emit mod_list_changed();
}

void ModListModel::set_conflict_order_reversed(bool reversed) {
    if (conflict_order_reversed_ == reversed) return;
    conflict_order_reversed_ = reversed;
}

QString ModListModel::compute_separator_flags(int row) const {
    bool has_wins = false, has_losses = false;
    for (int i = row + 1; i < mods_.size(); ++i) {
        if (mods_[i].is_separator) break;
        if (mods_[i].conflict_wins > 0) has_wins = true;
        if (mods_[i].conflict_losses > 0) has_losses = true;
    }
    if (has_wins && has_losses) return QString("\u00B1");
    if (has_wins) return QString("+");
    if (has_losses) return QString("-");
    return QString();
}

void ModListModel::set_conflict_pairs(const QMap<QString, ConflictPairs>& pairs) {
    conflict_pairs_ = pairs;
}

bool ModListModel::has_conflicts_within_separator(const QString& mod_id) const {
    QString sep_id;
    for (const auto& m : mods_) {
        if (m.id == mod_id) {
            sep_id = m.separator_id;
            break;
        }
    }
    if (sep_id.isEmpty()) return false;
    if (!conflict_pairs_.contains(mod_id)) return false;

    const auto& pairs = conflict_pairs_[mod_id];
    for (const auto& other : pairs.wins_against) {
        int idx = priority_of(other);
        if (idx >= 0 && mods_[idx].separator_id == sep_id) return true;
    }
    for (const auto& other : pairs.loses_to) {
        int idx = priority_of(other);
        if (idx >= 0 && mods_[idx].separator_id == sep_id) return true;
    }
    return false;
}

void ModListModel::set_selected_mod(const QString& id) {
    if (selected_mod_id_ == id) return;
    selected_mod_id_ = id;
    emit dataChanged(index(0, 0), index(mods_.size() - 1, ColumnCount - 1));
}

void ModListModel::set_overwrite_path(const QString& path) {
    if (overwrite_path_ == path) return;
    overwrite_path_ = path;
    // Refresh the Overwrite row's name color
    int ow_row = overwrite_row();
    if (ow_row >= 0)
        emit dataChanged(index(ow_row, Name), index(ow_row, Name), {Qt::ForegroundRole});
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
    int pos = mods_.size();  // always at bottom
    beginInsertRows({}, pos, pos);
    mods_.insert(pos, entry);
    endInsertRows();
}

int ModListModel::merged_row() const {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].is_merged) return i;
    }
    return -1;
}

bool ModListModel::is_merged(int row) const {
    return row >= 0 && row < mods_.size() && mods_[row].is_merged;
}

void ModListModel::ensure_merged_present() {
    for (const auto& m : mods_) {
        if (m.is_merged) return;
    }
    int overwrite_pos = overwrite_row();
    int pos = (overwrite_pos >= 0) ? overwrite_pos + 1 : mods_.size();
    ModEntry entry;
    entry.id = kMergedModId;
    entry.name = kMergedModName;
    entry.version = "";
    entry.enabled = true;
    entry.priority = 1;
    entry.is_merged = true;
    beginInsertRows({}, pos, pos);
    mods_.insert(pos, entry);
    endInsertRows();
}

}  // namespace ui
