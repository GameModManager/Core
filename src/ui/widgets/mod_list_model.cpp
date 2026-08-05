#include "ui/widgets/mod_list_model.h"
#include "ui/settings/settings.h"

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTreeView>

namespace ui {

// A 16x16 wizard hat (pointed purple hat with a yellow star), painted so the
// FOMOD indicator needs no asset file. Matches the conflict-icon cell size.
static QIcon make_wizard_hat_icon() {
    QPixmap pix(16, 16);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    // Brim
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(70, 40, 100));
    p.drawRoundedRect(QRectF(0.5, 11.5, 15, 3.5), 1.5, 1.5);
    // Pointed crown
    QPainterPath crown;
    crown.moveTo(4.5, 12);
    crown.lineTo(6.5, 3);
    crown.quadTo(7.0, 1.5, 9.5, 3);
    crown.lineTo(11.5, 12);
    crown.closeSubpath();
    p.setBrush(QColor(110, 60, 160));
    p.drawPath(crown);
    // Star
    QPainterPath star;
    star.moveTo(8, 5.5);
    star.lineTo(8.8, 7.4);
    star.lineTo(10.9, 7.5);
    star.lineTo(9.3, 8.8);
    star.lineTo(9.9, 10.9);
    star.lineTo(8, 9.8);
    star.lineTo(6.1, 10.9);
    star.lineTo(6.7, 8.8);
    star.lineTo(5.1, 7.5);
    star.lineTo(7.2, 7.4);
    star.closeSubpath();
    p.setBrush(QColor(255, 210, 80));
    p.drawPath(star);
    p.end();
    return QIcon(pix);
}

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
    fomod_icon_        = make_wizard_hat_icon();
    root_override_icon_ = QIcon(iconDir + "fugue/anchor.png");
}

int ModListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : mods_.size();
}

int ModListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ModListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= mods_.size()) return {};

    // ── Flag icons for the Flags column ─────────────────────────────
    // One individual QIcon per flag, in display order: conflict status, then
    // hidden-files badge, then FOMOD wizard marker. The FlagsDelegate paints
    // them one-by-one at native size and wraps to extra lines (growing the row)
    // when they exceed the column width — never stacked into one icon.
    if (role == kFlagIconsRole && index.column() == Flags) {
        const auto& m = mods_[index.row()];
        QList<QIcon> icons;
        if (m.is_separator) {
            auto flag = compute_separator_flags(index.row());
            if (flag == "+") icons << overwrite_icon_;
            else if (flag == "-") icons << overwritten_icon_;
            else if (flag == QString("\u00B1")) icons << mixed_icon_;
            return QVariant::fromValue(icons);
        }
        if (m.redundant) {
            icons << redundant_icon_;
        } else if (m.conflict_wins > 0 && m.conflict_losses > 0) {
            icons << mixed_icon_;
        } else if (m.conflict_wins > 0) {
            icons << overwrite_icon_;
        } else if (m.conflict_losses > 0) {
            icons << overwritten_icon_;
        }
        if (m.has_hidden_files) icons << hidden_icon_;
        if (m.is_fomod) icons << fomod_icon_;
        if (m.root_override) icons << root_override_icon_;
        return QVariant::fromValue(icons);
    }

    const auto& mod = mods_[index.row()];

    // --- Scroll mark color for the separator-marking scrollbar ---
    if (role == kScrollMarkRole) {
        if (mod.is_separator) {
            // Separator marks are gated by the "color separator scrollbar"
            // setting here (the scrollbar itself always draws marks); highlight
            // marks below are independent of it.
            if (!Settings::instance().color_separator_scrollbar()) return {};
            QColor bg(mod.separator_color.isEmpty() ? "#888888" : mod.separator_color);
            return bg;
        }
        // Plugin-selected highlight (MO2 "mod contains selected file") - feeds
        // the scrollbar mark so highlights are navigable in huge mod lists.
        if (highlighted_mods_.contains(mod.id))
            return Settings::instance().modlist_contains_file();
        return {};
    }

    // --- Separator: colored background spans all columns ---
    if (mod.is_separator) {
        if (role == Qt::BackgroundRole) {
            // Conflict highlight takes precedence if this separator is referenced
            if (!selected_mod_id_.isEmpty() && conflict_pairs_.contains(selected_mod_id_)) {
                const auto& pairs = conflict_pairs_[selected_mod_id_];
                if (pairs.wins_against.contains(mod.id))
                    return QBrush(Settings::instance().modlist_overwritten_loose());   // 30% green
                if (pairs.loses_to.contains(mod.id))
                    return QBrush(Settings::instance().modlist_overwriting_loose());   // 30% red
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
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
                // MO2-look: fold arrow is a prefix on the Name cell, followed
                // by the separator's display name (suffix already stripped).
                case Name: return (mod.folded ? QString("\u25B6 ") : QString("\u25BC ")) + mod.name;
                case Version: return QString();
                case Flags: return QString();  // icons come via kFlagIconsRole
                case Priority: return mod.priority;
            }
        }
        // EditRole carries the raw separator name (no arrow prefix) so
        // name-based lookups keep working.
        if (role == Qt::EditRole && index.column() == Name) return mod.name;
        if (role == Qt::TextAlignmentRole && index.column() == Priority) {
            return static_cast<int>(Qt::AlignCenter);
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

    // --- Overwrite: italic gray name, centered text ---
    if (mod.is_overwrite) {
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignCenter);
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
    if (role == Qt::CheckStateRole && index.column() == Name) {
        if (mod.is_overwrite || mod.is_merged || mod.is_game_native)
            return {};  // never-disableable rows carry no checkbox (MO2 parity)
        return mod.enabled ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case Name: return mod.name;
            case Version: return mod.version;
            case Flags: return QString();  // conflict/tag info is icon + tooltip
            case Priority: return mod.priority;
        }
    }
    if (role == Qt::ToolTipRole && index.column() == Flags &&
        (!mod.tags.isEmpty() || mod.is_fomod || mod.root_override)) {
        QStringList lines;
        if (mod.is_fomod) {
            lines << tr("FOMOD wizard: installed with selected options");
        }
        if (mod.root_override) {
            lines << tr("Deploys to the game root directory");
        }
        for (const auto& tag : mod.tags)
            lines << tr("%1: %2").arg(tag.type.toUpper(), tag.message);
        return lines.join("\n");
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
        // Plugin-selected highlight takes precedence over conflict colors
        // (MO2's markerColor beats overwrite markers).
        if (highlighted_mods_.contains(mod.id))
            return QBrush(Settings::instance().modlist_contains_file());
        if (!selected_mod_id_.isEmpty() && conflict_pairs_.contains(selected_mod_id_)) {
            const auto& pairs = conflict_pairs_[selected_mod_id_];
            if (pairs.wins_against.contains(mod.id))
                return QBrush(Settings::instance().modlist_overwritten_loose());
            if (pairs.loses_to.contains(mod.id))
                return QBrush(Settings::instance().modlist_overwriting_loose());
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
    if (m.is_overwrite || m.is_merged || m.is_game_native) return false;

    if (role == Qt::CheckStateRole && index.column() == Name) {
        if (m.is_separator) return false;
        mods_[index.row()].enabled = (value.toInt() == Qt::Checked);
        emit dataChanged(index, index, {Qt::CheckStateRole});
        emit mod_list_changed();
        return true;
    }

    // Inline rename (MO2 renameMod): the window handler does the disk rename
    // synchronously and updates the row; it reverts via dataChanged on failure.
    if (role == Qt::EditRole && index.column() == Name) {
        emit rename_requested(index.row(), value.toString().trimmed());
        return true;
    }
    return false;
}

QVariant ModListModel::headerData(int section, Qt::Orientation, int role) const {
    if (role != Qt::DisplayRole) return {};
    switch (section) {
        case Name: return tr("Name");
        case Version: return tr("Version");
        case Flags: return tr("Flags");
        case Priority: return tr("Priority");
    }
    return {};
}

Qt::ItemFlags ModListModel::flags(const QModelIndex& index) const {
    auto f = QAbstractTableModel::flags(index);
    if (!index.isValid()) return f | Qt::ItemIsDropEnabled;

    const auto& mod = mods_[index.row()];

    if (mod.is_separator) {
        f &= ~Qt::ItemIsUserCheckable;
        f |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
        if (index.column() == Name)
            f |= Qt::ItemIsEditable;
        return f;
    }

    if (index.column() == Name) {
        if (!mod.is_overwrite && !mod.is_merged && !mod.is_game_native) {
            f |= Qt::ItemIsUserCheckable;
            f |= Qt::ItemIsEditable;
        }
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
            if (r < mods_.size() && !mods_[r].is_overwrite && !mods_[r].is_merged && !mods_[r].is_game_native) {
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
    } else if (row < 0) {
        // Drop onto empty viewport space (OnViewport): append at the end.
        // The Overwrite / game-native clamps below still apply.
        row = rowCount({});
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
        if (r >= 0 && r < mods_.size() && !mods_[r].is_overwrite && !mods_[r].is_merged && !mods_[r].is_game_native) {
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

    // Never drop into the game-native band (unmanaged mods stay on top)
    int native_bottom = native_band_bottom();
    if (targetRow < native_bottom)
        targetRow = native_bottom;

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

    if (mods_[srcRow].is_overwrite || mods_[srcRow].is_merged || mods_[srcRow].is_game_native) return false;

    int dest = dstRow > srcRow ? dstRow - 1 : dstRow;
    // Prevent moving onto or past Overwrite or MERGED (always pinned)
    int ow_row = overwrite_row();
    if (ow_row >= 0 && dest >= ow_row)
        dest = ow_row - 1;
    int mg_row = merged_row();
    if (mg_row >= 0 && dest >= mg_row)
        dest = mg_row - 1;
    // Never move into the game-native band (unmanaged mods stay on top)
    int native_bottom = native_band_bottom();
    if (dest < native_bottom)
        dest = native_bottom;
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
    // MO2 rule (Profile::refreshModStatus): a new mod that isn't in the mod
    // list yet gets the HIGHEST regular priority - placed at the bottom of the
    // user band, directly above the pinned MERGED/Overwrite block, never past
    // Overwrite. Game-native mods and explicit-priority adds append at the
    // end; load_order() is the final arbiter of display order.
    if (!is_game_native && priority < 0) {
        int mg_row = merged_row();
        int ow_row = overwrite_row();
        if (mg_row >= 0) {
            insert_pos = mg_row;
        } else if (ow_row >= 0) {
            insert_pos = ow_row;
        }
    }
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

void ModListModel::rename_mod_in_place(int row, const QString& new_id, const QString& new_name) {
    if (row < 0 || row >= mods_.size()) return;
    if (mods_[row].is_overwrite || mods_[row].is_merged) return;
    mods_[row].id = new_id;
    mods_[row].name = new_name;
    emit dataChanged(index(row, Name), index(row, Version), {Qt::DisplayRole, Qt::EditRole});
    emit mod_list_changed();
}

void ModListModel::set_mod_color(const QString& id, const QColor& color) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].separator_color =
                color.isValid() ? color.name(QColor::HexArgb) : QString();
            emit dataChanged(index(i, Name), index(i, Priority));
            emit mod_list_changed();
            return;
        }
    }
}

void ModListModel::clear_mod_color(const QString& id) {
    set_mod_color(id, QColor());
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
    if (mods_[src].is_game_native) return;

    int ow_row = overwrite_row();
    int mg_row = merged_row();
    // Clamp to just before Overwrite/MERGED - after takeAt(src) they shift left by 1
    if (mg_row >= 0 && new_row >= mg_row)
        new_row = mg_row - 1;
    if (ow_row >= 0 && new_row >= ow_row)
        new_row = ow_row - 1;
    // Never move into the game-native band (unmanaged mods stay on top)
    int native_bottom = native_band_bottom();
    if (new_row < native_bottom)
        new_row = native_bottom;
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
            emit dataChanged(index(i, Name), index(i, Name), {Qt::CheckStateRole});
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
                             {Qt::SizeHintRole, kFlagIconsRole, Qt::ToolTipRole});
            return;
        }
    }
}

void ModListModel::set_conflict_redundant(const QString& id, bool redundant) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].redundant != redundant) {
            mods_[i].redundant = redundant;
            emit dataChanged(index(i, Flags), index(i, Flags),
                             {Qt::SizeHintRole, kFlagIconsRole});
            return;
        }
    }
}

void ModListModel::set_hidden_files(const QString& id, bool has_hidden) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].has_hidden_files != has_hidden) {
            mods_[i].has_hidden_files = has_hidden;
            emit dataChanged(index(i, Flags), index(i, Flags),
                             {Qt::SizeHintRole, kFlagIconsRole});
            return;
        }
    }
}

void ModListModel::set_fomod(const QString& id, bool on) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].is_fomod != on) {
            mods_[i].is_fomod = on;
            emit dataChanged(index(i, Flags), index(i, Flags),
                             {Qt::SizeHintRole, kFlagIconsRole, Qt::ToolTipRole});
            return;
        }
    }
}

void ModListModel::set_root_override(const QString& id, bool on) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].root_override != on) {
            mods_[i].root_override = on;
            emit dataChanged(index(i, Flags), index(i, Flags),
                             {Qt::SizeHintRole, kFlagIconsRole, Qt::ToolTipRole});
            return;
        }
    }
}

void ModListModel::set_tags(const QString& id, const QVector<ModTag>& tags) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].tags = tags;
            emit dataChanged(index(i, Flags), index(i, Flags),
                             {Qt::SizeHintRole, kFlagIconsRole, Qt::ToolTipRole});
            return;
        }
    }
}

void ModListModel::set_source_info(const QString& id, const QString& source_type, const QString& source_id, const QString& page_url) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id) {
            mods_[i].source_type = source_type;
            mods_[i].source_id = source_id;
            mods_[i].source_page_url = page_url;
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
    emit dataChanged(index(row, Name), index(row, Name));
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

void ModListModel::set_highlighted_mods(const QSet<QString>& ids) {
    if (highlighted_mods_ == ids) return;
    highlighted_mods_ = ids;
    // One dataChanged over the full range repaints the visible rows and the
    // scrollbar marks (ModMarkingScrollBar listens to dataChanged).
    emit dataChanged(index(0, 0), index(mods_.size() - 1, ColumnCount - 1),
                     {Qt::BackgroundRole, kScrollMarkRole});
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

int ModListModel::native_band_bottom() const {
    int i = 0;
    while (i < mods_.size() && mods_[i].is_game_native) ++i;
    return i;
}

bool ModListModel::is_merged(int row) const {
    return row >= 0 && row < mods_.size() && mods_[row].is_merged;
}

void ModListModel::set_uses_merged(bool on) {
    if (uses_merged_ == on) return;
    uses_merged_ = on;
    if (on) {
        ensure_merged_present();
    } else {
        int row = merged_row();
        if (row >= 0) {
            beginRemoveRows({}, row, row);
            mods_.removeAt(row);
            endRemoveRows();
            renumber_priorities();
            emit mod_list_changed();
        }
    }
}

void ModListModel::ensure_merged_present() {
    // Only games that use the merged pseudo-mod (currently Isaac) pin it.
    if (!uses_merged_) return;
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
