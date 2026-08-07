#pragma once

#include <QAbstractTableModel>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QSet>
#include <QVector>

class QAbstractItemView;

namespace ui {

constexpr const char* kOverwriteModId = "__overwrite__";
constexpr const char* kOverwriteModName = "overwrite";
constexpr const char* kMergedModId = "__merged__";
constexpr const char* kMergedModName = "MERGED";

constexpr const char* kModListMimeType = "application/x-gmm-modlist";

struct ModTag {
    QString type;    // "deprecated", "note", "warning", "incompatible", "clean", "dirty"
    QString message; // The message to display
};

struct ModEntry {
    QString id;
    QString name;
    QString version;
    QString separator_color;
    bool enabled = true;
    int priority = 0;
    int conflict_wins = 0;
    int conflict_losses = 0;
    bool redundant = false;       // every file this mod provides is overridden by a higher-priority owner
    bool has_hidden_files = false;  // some files are hidden by the (not yet implemented) hidden-files feature
    QVector<ModTag> tags;
    QString source_type;
    QString source_id;
    // Source page URL persisted in the mod's per-source meta section
    // (LoversLab: the page the download came from). Used by "Visit on ...".
    QString source_page_url;
    QString separator_id;
    bool is_separator = false;
    bool is_overwrite = false;
    bool is_merged = false;
    bool is_game_native = false;
    bool is_fomod = false;        // installed via the FOMOD wizard
    bool root_override = false;   // deploys to the game root instead of the data dir
    bool invalid_data = false;    // MO2 FLAG_INVALID: folder holds no recognized game data
    bool no_metadata = false;     // no manager metadata file in the folder (not a managed install)
    bool folded = false;
    // Category name resolved from the instance's category DB (meta.ini
    // [General] category CSV primary, else [Nexusmods] nexuscategory). Empty
    // when unresolvable.
    QString category;
    // Installation (folder birth time) and Changed (folder last-write time).
    // 0 = unavailable (separators, Overwrite/MERGED pseudo-rows).
    qint64 installation_ts = 0;
    qint64 changed_ts = 0;
};

struct ConflictPairs {
    QStringList wins_against;
    QStringList loses_to;
};

class ModListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    // Column order is the display order: Fold first (never hideable, pinned to
    // the left edge, carries the separator fold arrow), Name second, Priority
    // last. Adding/reordering columns is safe - no code persists column
    // indices.
    enum Column { Fold, Name, Conflicts, Flags, Category, Source, SourceId,
                  Version, Installation, Changed, Priority, ColumnCount };

    // Custom role for the separator-marking scrollbar; separator rows return
    // their background QColor, everything else returns an invalid variant.
    static constexpr int kScrollMarkRole = Qt::UserRole + 1;

    // Individual flag icons for the Flags column (conflict status, hidden-files,
    // FOMOD saved badge), as QList<QIcon>. The FlagsDelegate paints each at
    // native size and wraps to extra lines (growing the row) when they exceed the
    // column width, instead of stacking them into one squeezed icon.
    static constexpr int kFlagIconsRole = Qt::UserRole + 2;

    explicit ModListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
                      int row, int column, const QModelIndex& parent) override;
    bool moveRows(const QModelIndex& srcParent, int srcRow, int count,
                  const QModelIndex& dstParent, int dstRow) override;

    void add_mod(const QString& id, const QString& name, const QString& version,
                 int priority = -1, bool is_game_native = false,
                 qint64 install_ts = 0, qint64 changed_ts = 0);
    void add_separator(const QString& id, const QString& name, const QString& color);
    void remove_mod(const QString& id);
    void remove_all_mods();
    void move_mod(const QString& id, int new_row);
    void toggle_mod(const QString& id);
    // In-place rename that keeps the row where it is (id/priority/position
    // unchanged except the id + display name). MO2 renames the folder on disk,
    // so the id (folder name) changes too - this just updates the row.
    void rename_mod_in_place(int row, const QString& new_id, const QString& new_name);
    // Separator row colors (full-row tint). Invalid QColor clears the color.
    void set_mod_color(const QString& id, const QColor& color);
    void clear_mod_color(const QString& id);
    void set_conflict_stats(const QString& id, int wins, int losses);
    void set_conflict_redundant(const QString& id, bool redundant);
    void set_hidden_files(const QString& id, bool has_hidden);
    void set_fomod(const QString& id, bool on);
    void set_root_override(const QString& id, bool on);
    // MO2 FLAG_INVALID / missing-metadata markers (see ModEntry).
    void set_invalid_data(const QString& id, bool on);
    void set_no_metadata(const QString& id, bool on);
    void set_tags(const QString& id, const QVector<ModTag>& tags);
    void set_source_info(const QString& id, const QString& source_type,
                         const QString& source_id,
                         const QString& page_url = {});
    // Category name for the Category column (resolved by the window layer from
    // the instance's category DB; the model just stores the display string).
    void set_category(const QString& id, const QString& category);
    // Installation (folder birth time) + Changed (folder last-write time),
    // epoch seconds. 0 clears both cells.
    void set_timestamps(const QString& id, qint64 install_ts, qint64 changed_ts);
    void set_separator_id(const QString& id, const QString& separator_id);
    void set_priority(const QString& id, int priority);
    void renumber_priorities();
    void set_folded(int row, bool folded);
    void apply_fold_state();
    QStringList existing_separator_names() const;

    [[nodiscard]] const QVector<ModEntry>& mods() const { return mods_; }
    [[nodiscard]] QStringList enabled_mod_ids() const;
    [[nodiscard]] int priority_of(const QString& id) const;
    [[nodiscard]] int overwrite_row() const;
    [[nodiscard]] bool is_overwrite(int row) const;
    [[nodiscard]] int merged_row() const;
    [[nodiscard]] bool is_merged(int row) const;
    // Game-native (unmanaged) band rows. The band is contiguous (clamps keep
    // it that way) but is NOT necessarily leading: a separator may sit above
    // it (so its fold can hide the native mods). native_band_first() is the
    // first native row (== mods_.size() when there is no native band);
    // native_band_last() is the last native row (-1 when none). Only
    // separators may be placed at or above the band.
    [[nodiscard]] int native_band_first() const;
    [[nodiscard]] int native_band_last() const;
    // Whether a row carries a fold arrow ("has content to hide"). For a
    // separator this is the flat band rule: there is at least one hideable row
    // (mod/native/merged) below it before the next separator or Overwrite.
    // Every other row type returns false; mod submods are a future feature.
    [[nodiscard]] bool separator_has_content(int row) const;
    [[nodiscard]] bool uses_merged() const { return uses_merged_; }

    void set_view(QAbstractItemView* view) { mod_view_ = view; }
    void reset_with_order(const QVector<ModEntry>& entries);
    void set_conflict_order_reversed(bool reversed);
    void set_uses_merged(bool on);
    [[nodiscard]] bool is_conflict_order_reversed() const { return conflict_order_reversed_; }

    void set_conflict_pairs(const QMap<QString, ConflictPairs>& pairs);
    [[nodiscard]] const QMap<QString, ConflictPairs>& conflict_pairs() const { return conflict_pairs_; }
    [[nodiscard]] bool has_conflicts_within_separator(const QString& mod_id) const;
    void set_selected_mod(const QString& id);
    // Mark mods owning a plugin selected in the plugins list (MO2's
    // "Mod contains selected file" highlight). Rows render modlist_contains_file
    // and feed the scrollbar marks. Empty set clears.
    void set_highlighted_mods(const QSet<QString>& ids);
    void set_overwrite_path(const QString& path);
    [[nodiscard]] QString overwrite_path() const { return overwrite_path_; }
    void ensure_merged_present();

signals:
    void mod_list_changed();
    // Emitted by setData(Qt::EditRole) on the Name column. The window handler
    // performs the disk rename (folder rename + metadata); on failure it must
    // restore the row and emit dataChanged so the editor is reverted.
    void rename_requested(int row, const QString& name);

private:
    void ensure_overwrite_present();
    [[nodiscard]] QString compute_separator_flags(int row) const;
    // Vendor badge for a mod's source_type (via engine::vendor_icon_key), or
    // null for unknown/empty sources.
    [[nodiscard]] QIcon source_icon(const QString& source_type) const;

    QVector<ModEntry> mods_;
    QIcon overwrite_icon_;
    QIcon overwritten_icon_;
    QIcon mixed_icon_;
    QIcon redundant_icon_;
    QIcon hidden_icon_;
    QIcon fomod_icon_;
    QIcon root_override_icon_;
    QIcon invalid_icon_;
    // source icon-key ("nexusmods", "loverslab", "steam", "moddb") -> badge.
    QHash<QString, QIcon> vendor_icons_;
    QAbstractItemView* mod_view_ = nullptr;
    bool conflict_order_reversed_ = false;
    bool uses_merged_ = false;
    QString selected_mod_id_;
    QSet<QString> highlighted_mods_;
    QMap<QString, ConflictPairs> conflict_pairs_;
    QString overwrite_path_;
};

}  // namespace ui
