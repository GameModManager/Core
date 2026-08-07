#include "ui/widgets/mod_list_model.h"
#include "ui/settings/settings.h"

#include "engine/theme/icon_manager.h"

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QMimeData>
#include <QPixmap>
#include <QTreeView>

namespace ui {

// Epoch seconds -> "yyyy-MM-dd HH:mm:ss" for the Installation/Changed columns.
static QString format_epoch_ts(qint64 ts) {
    return QDateTime::fromSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss");
}

ModListModel::ModListModel(QObject* parent)
    : QAbstractTableModel(parent) {
    ensure_overwrite_present();
    ensure_merged_present();

    // Conflict-status icons resolve through IconManager so a theme or icon
    // pack can override them (the "mo2" pack ships MO2-style badges).
    auto& icons = engine::IconManager::instance();
    overwrite_icon_    = icons.resolve_icon("conflict-overwrite");
    overwritten_icon_  = icons.resolve_icon("conflict-overwritten");
    mixed_icon_        = icons.resolve_icon("conflict-mixed");
    redundant_icon_    = icons.resolve_icon("conflict-redundant");
    hidden_icon_       = icons.resolve_icon("conflict-hidden");
    fomod_icon_        = icons.resolve_icon("save");
    root_override_icon_ = icons.resolve_icon("root-dir");
    invalid_icon_       = icons.resolve_icon("mod-invalid");
    // Vendor icons for the Source column (MO2 COL_GAME analogue): resolved
    // through the same vendor_icon_key() mapping the Source tab uses.
    for (const char* key : {"nexusmods", "loverslab", "steam", "moddb"}) {
        QIcon icon = icons.resolve_icon(QString::fromLatin1(key));
        if (!icon.isNull()) vendor_icons_[QString::fromLatin1(key)] = icon;
    }
}

int ModListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : mods_.size();
}

int ModListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ModListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= mods_.size()) return {};

    // ── Flag icons for the Conflicts/Flags columns ────────────────────────
    // MO2 keeps these as two columns: COL_CONFLICTFLAGS carries the win/loss
    // state badge (and a separator's +/-/± merge-state icon), COL_FLAGS the
    // hidden-files/FOMOD/root-override/invalid badges. Each is an individual
    // QIcon under kFlagIconsRole; the FlagsDelegate paints them one-by-one at
    // native size and wraps to extra lines (growing the row) when they exceed
    // the column width — never stacked into one icon.
    if (role == kFlagIconsRole &&
        (index.column() == Conflicts || index.column() == Flags)) {
        const auto& m = mods_[index.row()];
        QList<QIcon> icons;
        if (index.column() == Conflicts) {
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
            return QVariant::fromValue(icons);
        }
        if (m.has_hidden_files) icons << hidden_icon_;
        if (m.is_fomod) icons << fomod_icon_;
        if (m.root_override) icons << root_override_icon_;
        if (m.invalid_data || m.no_metadata) icons << invalid_icon_;
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
        if (role == Qt::FontRole && (index.column() == Name || index.column() == Fold)) {
            QFont f;
            f.setBold(true);
            return f;
        }
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
                // Fold arrow lives in its own column (pinned left of Name).
                // It renders only when the separator has content to hide
                // (band rule); an empty band shows a dead cell. No trailing
                // space - the cell centers the glyph on its own.
                case Fold:
                    return separator_has_content(index.row())
                               ? (mod.folded ? QString("\u25B6") : QString("\u25BC"))
                               : QString();
                case Name: return mod.name;
                case Version: return QString();
                case Flags: return QString();  // icons come via kFlagIconsRole
                case Priority: return mod.priority;
            }
        }
        // EditRole carries the raw separator name so name-based lookups keep
        // working (the fold arrow never enters the name in any role).
        if (role == Qt::EditRole && index.column() == Name) return mod.name;
        if (role == Qt::TextAlignmentRole) {
            // The Fold column is always centered (arrow pinned to the edge,
            // centered within its cell) regardless of the center-separators
            // setting.
            if (index.column() == Fold)
                return static_cast<int>(Qt::AlignCenter);
            // "Center text on separators" (Settings > Theme > Design, default
            // on) centers every separator cell; the Priority cell stays
            // centered regardless (MO2 parity).
            if (Settings::instance().center_separator_text())
                return static_cast<int>(Qt::AlignCenter);
            if (index.column() == Priority)
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

    // --- MO2 FLAG_INVALID: italic dark-gray name ("No valid game data") ---
    if (mod.invalid_data) {
        if (role == Qt::FontRole && index.column() == Name) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        if (role == Qt::ForegroundRole && index.column() == Name) {
            return QColor(140, 140, 140);
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
            case Conflicts: return QString();  // icon via kFlagIconsRole
            case Flags: return QString();      // icon via kFlagIconsRole
            case Category: return mod.category;
            case Source: return QString();     // vendor icon via DecorationRole
            case SourceId: return mod.source_id;
            case Version: return mod.version;
            case Installation:
                return mod.installation_ts > 0 ? format_epoch_ts(mod.installation_ts)
                                               : QString();
            case Changed:
                return mod.changed_ts > 0 ? format_epoch_ts(mod.changed_ts) : QString();
            case Priority: return mod.priority;
        }
    }
    // Vendor icon for the Source column (MO2 COL_GAME analogue): the badge of
    // the site the download came from (Nexus/LoversLab/Steam/ModDB).
    if (role == Qt::DecorationRole && index.column() == Source && !mod.is_separator) {
        return source_icon(mod.source_type);
    }
    // Conflicts tooltip: what this mod wins/loses against.
    if (role == Qt::ToolTipRole && index.column() == Conflicts && !mod.is_separator &&
        (mod.conflict_wins > 0 || mod.conflict_losses > 0 || mod.redundant)) {
        QStringList lines;
        if (mod.redundant)
            lines << tr("Redundant: every file is provided by a higher-priority mod");
        if (mod.conflict_wins > 0)
            lines << tr("Overwrites %1 file(s)").arg(mod.conflict_wins);
        if (mod.conflict_losses > 0)
            lines << tr("Overwritten by %1 file(s)").arg(mod.conflict_losses);
        return lines.join("\n");
    }
    // Source tooltip: the download source name.
    if (role == Qt::ToolTipRole && index.column() == Source && !mod.source_type.isEmpty())
        return mod.source_type;
    if (role == Qt::ToolTipRole && index.column() == Flags &&
        (!mod.tags.isEmpty() || mod.is_fomod || mod.root_override ||
         mod.invalid_data || mod.no_metadata)) {
        QStringList lines;
        if (mod.invalid_data) {
            lines << tr("No valid game data");
        }
        if (mod.no_metadata) {
            lines << tr("Not installed by the manager (no metadata file)");
        }
        if (mod.is_fomod) {
            lines << tr("FOMOD saved: installed with selected options");
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
    // Centered text/icon columns (MO2 centers everything but Name/Version).
    // Fold is always centered (arrow cell); every other Fold cell is empty.
    if (role == Qt::TextAlignmentRole &&
        (index.column() == Fold || index.column() == Conflicts ||
         index.column() == Category || index.column() == SourceId ||
         index.column() == Installation || index.column() == Changed ||
         index.column() == Priority)) {
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
        case Conflicts: return tr("Conflicts");
        case Flags: return tr("Flags");
        case Category: return tr("Category");
        case Source: return tr("Source");
        case SourceId: return tr("Source ID");
        case Version: return tr("Version");
        case Installation: return tr("Installation");
        case Changed: return tr("Changed");
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
    bool all_separators = true;
    for (int r : sourceRows) {
        if (r >= 0 && r < mods_.size() && !mods_[r].is_overwrite && !mods_[r].is_merged && !mods_[r].is_game_native) {
            validSources.append(r);
            if (!mods_[r].is_separator) all_separators = false;
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

    // Game-native (unmanaged) mods stay on top. Only a separator may enter
    // the band region (so its fold can hide the native mods); an in-band
    // separator drop snaps to just above the band to keep it contiguous. Any
    // non-separator in the drag forces the whole drop below the band.
    if (all_separators) {
        int nb_first = native_band_first();
        int nb_last = native_band_last();
        if (nb_first >= 0 && targetRow > nb_first && targetRow <= nb_last)
            targetRow = nb_first;
    } else {
        int nb_last = native_band_last();
        if (nb_last >= 0 && targetRow <= nb_last)
            targetRow = nb_last + 1;
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

    if (mods_[srcRow].is_overwrite || mods_[srcRow].is_merged || mods_[srcRow].is_game_native) return false;

    int dest = dstRow > srcRow ? dstRow - 1 : dstRow;
    // Prevent moving onto or past Overwrite or MERGED (always pinned)
    int ow_row = overwrite_row();
    if (ow_row >= 0 && dest >= ow_row)
        dest = ow_row - 1;
    int mg_row = merged_row();
    if (mg_row >= 0 && dest >= mg_row)
        dest = mg_row - 1;
    // Game-native (unmanaged) mods stay on top. Only a separator may enter
    // the band region (so its fold can hide the native mods); an in-band
    // separator move snaps to just above the band to keep it contiguous.
    // dest is in post-removal coordinates, so the band bounds shift up by one
    // when the moved row sat at or above the band.
    int nb_first = native_band_first();
    int nb_last = native_band_last();
    if (srcRow <= nb_first) {
        nb_first -= 1;
        nb_last -= 1;
    }
    if (mods_[srcRow].is_separator) {
        if (nb_first >= 0 && dest > nb_first && dest <= nb_last)
            dest = nb_first;
    } else if (nb_last >= 0 && dest <= nb_last) {
        dest = nb_last + 1;
    }
    if (dest < 0) dest = 0;
    // The clamps above can pull dest back onto srcRow (a no-op move); report
    // it as such - Qt's movePersistentIndexes crashes on a self-move.
    if (dest == srcRow) return false;

    beginMoveRows(srcParent, srcRow, srcRow, srcParent, dest + (dest >= srcRow ? 1 : 0));
    auto item = mods_.takeAt(srcRow);
    mods_.insert(dest, std::move(item));
    endMoveRows();

    renumber_priorities();
    emit mod_list_changed();
    return true;
}

void ModListModel::add_mod(const QString& id, const QString& name, const QString& version, int priority, bool is_game_native, qint64 install_ts, qint64 changed_ts) {
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
    entry.installation_ts = install_ts;
    entry.changed_ts = changed_ts;
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
    // Game-native (unmanaged) mods stay on top. Only a separator may enter
    // the band region (so its fold can hide the native mods); an in-band
    // separator move snaps to just above the band to keep it contiguous.
    // new_row is in post-removal coordinates, so the band bounds shift up by
    // one when the moved row sat at or above the band.
    int nb_first = native_band_first();
    int nb_last = native_band_last();
    if (src <= nb_first) {
        nb_first -= 1;
        nb_last -= 1;
    }
    if (mods_[src].is_separator) {
        if (nb_first >= 0 && new_row > nb_first && new_row <= nb_last)
            new_row = nb_first;
    } else if (nb_last >= 0 && new_row <= nb_last) {
        new_row = nb_last + 1;
    }
    if (new_row < 0) new_row = 0;
    // The clamps above can pull new_row back onto src (a no-op move); report
    // it as such - Qt's movePersistentIndexes crashes on a self-move.
    if (new_row == src) return;

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

void ModListModel::set_invalid_data(const QString& id, bool on) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].invalid_data != on) {
            mods_[i].invalid_data = on;
            // Name styling (italic gray) + Flags icon/tooltip both change.
            emit dataChanged(index(i, Name), index(i, Flags),
                             {Qt::FontRole, Qt::ForegroundRole, Qt::SizeHintRole,
                              kFlagIconsRole, Qt::ToolTipRole});
            return;
        }
    }
}

void ModListModel::set_no_metadata(const QString& id, bool on) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].no_metadata != on) {
            mods_[i].no_metadata = on;
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
            if (mods_[i].source_type != source_type || mods_[i].source_id != source_id ||
                mods_[i].source_page_url != page_url) {
                mods_[i].source_type = source_type;
                mods_[i].source_id = source_id;
                mods_[i].source_page_url = page_url;
                // Source icon + Source ID text both derive from this.
                emit dataChanged(index(i, Source), index(i, SourceId),
                                 {Qt::DisplayRole, Qt::DecorationRole, Qt::ToolTipRole});
            }
            return;
        }
    }
}

void ModListModel::set_category(const QString& id, const QString& category) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id && mods_[i].category != category) {
            mods_[i].category = category;
            emit dataChanged(index(i, Category), index(i, Category));
            return;
        }
    }
}

void ModListModel::set_timestamps(const QString& id, qint64 install_ts, qint64 changed_ts) {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].id == id &&
            (mods_[i].installation_ts != install_ts || mods_[i].changed_ts != changed_ts)) {
            mods_[i].installation_ts = install_ts;
            mods_[i].changed_ts = changed_ts;
            emit dataChanged(index(i, Installation), index(i, Changed));
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
    // Structural changes (add/remove/move) can flip a separator's "has
    // content" band and therefore its arrow; the affected separator rows are
    // not repainted by the row insert/remove/move alone, so refresh the whole
    // Fold column.
    if (!mods_.isEmpty())
        emit dataChanged(index(0, Fold), index(mods_.size() - 1, Fold), {Qt::DisplayRole});
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
    // Repaint both the Fold cell (glyph flips \u25BC <-> \u25B6) and the Name
    // cell it used to prefix.
    emit dataChanged(index(row, Fold), index(row, Name), {Qt::DisplayRole});
    apply_fold_state();
    // Persist the fold: the MainWindow mod_list_changed handler saves
    // instance.toml's folded_separators. Without this a fold/unfold never
    // reached the disk and the state reset on every reopen/relaunch.
    emit mod_list_changed();
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

QIcon ModListModel::source_icon(const QString& source_type) const {
    if (source_type.isEmpty()) return {};
    const QString key =
        QString::fromStdString(engine::vendor_icon_key(source_type.toStdString()));
    if (key.isEmpty()) return {};
    return vendor_icons_.value(key);
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

bool ModListModel::separator_has_content(int row) const {
    if (row < 0 || row >= mods_.size()) return false;
    if (!mods_[row].is_separator) return false;
    // Flat band rule: content exists if any hideable row (mod/native/merged -
    // everything apply_fold_state hides) sits between this separator and the
    // next separator or Overwrite. The scan stops at the first one, so this is
    // O(1)-ish per call.
    for (int i = row + 1; i < mods_.size(); ++i) {
        if (mods_[i].is_separator || mods_[i].is_overwrite) return false;
        return true;
    }
    return false;
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

int ModListModel::native_band_first() const {
    for (int i = 0; i < mods_.size(); ++i) {
        if (mods_[i].is_game_native) return i;
    }
    return mods_.size();
}

int ModListModel::native_band_last() const {
    for (int i = mods_.size() - 1; i >= 0; --i) {
        if (mods_[i].is_game_native) return i;
    }
    return -1;
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
