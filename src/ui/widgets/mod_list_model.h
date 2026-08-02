#pragma once

#include <QAbstractTableModel>
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
    QString separator_id;
    bool is_separator = false;
    bool is_overwrite = false;
    bool is_merged = false;
    bool is_game_native = false;
    bool folded = false;
};

struct ConflictPairs {
    QStringList wins_against;
    QStringList loses_to;
};

class ModListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Version, Flags, Priority, ColumnCount };

    // Custom role for the separator-marking scrollbar; separator rows return
    // their background QColor, everything else returns an invalid variant.
    static constexpr int kScrollMarkRole = Qt::UserRole + 1;

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

    void add_mod(const QString& id, const QString& name, const QString& version, int priority = -1, bool is_game_native = false);
    void add_separator(const QString& id, const QString& name, const QString& color);
    void remove_mod(const QString& id);
    void remove_all_mods();
    void move_mod(const QString& id, int new_row);
    void toggle_mod(const QString& id);
    void set_conflict_stats(const QString& id, int wins, int losses);
    void set_conflict_redundant(const QString& id, bool redundant);
    void set_hidden_files(const QString& id, bool has_hidden);
    void set_tags(const QString& id, const QVector<ModTag>& tags);
    void set_source_info(const QString& id, const QString& source_type, const QString& source_id);
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
    // Bottom of the leading game-native (unmanaged) band: the first row that is
    // not game-native. Everything above it is pinned and never reorderable.
    [[nodiscard]] int native_band_bottom() const;
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

private:
    void ensure_overwrite_present();
    [[nodiscard]] QString compute_separator_flags(int row) const;

    QVector<ModEntry> mods_;
    QIcon overwrite_icon_;
    QIcon overwritten_icon_;
    QIcon mixed_icon_;
    QIcon redundant_icon_;
    QIcon hidden_icon_;
    QAbstractItemView* mod_view_ = nullptr;
    bool conflict_order_reversed_ = false;
    bool uses_merged_ = false;
    QString selected_mod_id_;
    QSet<QString> highlighted_mods_;
    QMap<QString, ConflictPairs> conflict_pairs_;
    QString overwrite_path_;
};

}  // namespace ui
