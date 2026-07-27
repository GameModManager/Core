#pragma once

#include <QAbstractTableModel>
#include <QVector>

class QAbstractItemView;

namespace ui {

constexpr const char* kOverwriteModId = "__overwrite__";
constexpr const char* kOverwriteModName = "Overwrite";

constexpr const char* kModListMimeType = "application/x-gmm-modlist";

struct ModEntry {
    QString id;
    QString name;
    QString version;
    QString separator_color;
    bool enabled = true;
    int priority = 0;
    QStringList conflicts;
    bool is_separator = false;
    bool is_overwrite = false;
    bool folded = false;
};

class ModListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Enabled, Name, Version, Flags, Priority, ColumnCount };

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

    void add_mod(const QString& id, const QString& name, const QString& version, int priority = -1);
    void add_separator(const QString& id, const QString& name, const QString& color);
    void remove_mod(const QString& id);
    void toggle_mod(const QString& id);
    void set_conflicts(const QString& id, const QStringList& conflicting_ids);
    void renumber_priorities();
    void set_folded(int row, bool folded);
    void apply_fold_state();
    QStringList existing_separator_names() const;

    [[nodiscard]] const QVector<ModEntry>& mods() const { return mods_; }
    [[nodiscard]] QStringList enabled_mod_ids() const;
    [[nodiscard]] int priority_of(const QString& id) const;
    [[nodiscard]] int overwrite_row() const;
    [[nodiscard]] bool is_overwrite(int row) const;

    void set_view(QAbstractItemView* view) { mod_view_ = view; }
    void reset_with_order(const QVector<ModEntry>& entries);
    void set_conflict_order_reversed(bool reversed);

signals:
    void mod_list_changed();

private:
    void ensure_overwrite_present();

    QVector<ModEntry> mods_;
    QAbstractItemView* mod_view_ = nullptr;
    bool conflict_order_reversed_ = false;
};

}  // namespace ui
