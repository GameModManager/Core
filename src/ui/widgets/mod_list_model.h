#pragma once

#include <QAbstractTableModel>
#include <QVector>

namespace ui {

struct ModEntry {
    QString id;
    QString name;
    QString version;
    bool enabled = true;
    int priority = 0;
    QStringList conflicts;
};

class ModListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Enabled, Name, Version, Status, Priority, ColumnCount };

    explicit ModListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    Qt::DropActions supportedDropActions() const override;
    bool moveRows(const QModelIndex& srcParent, int srcRow, int count,
                  const QModelIndex& dstParent, int dstRow) override;

    void add_mod(const QString& id, const QString& name, const QString& version);
    void remove_mod(const QString& id);
    void toggle_mod(const QString& id);
    void set_conflicts(const QString& id, const QStringList& conflicting_ids);
    void renumber_priorities();

    [[nodiscard]] const QVector<ModEntry>& mods() const { return mods_; }
    [[nodiscard]] QStringList enabled_mod_ids() const;
    [[nodiscard]] int priority_of(const QString& id) const;

signals:
    void mod_list_changed();

private:
    QVector<ModEntry> mods_;
};

}  // namespace ui
