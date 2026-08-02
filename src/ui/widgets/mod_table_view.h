#pragma once

#include <QScrollBar>
#include <QTreeView>
#include <QStringList>

class QAbstractItemModel;

namespace ui {

class ModListModel;

// Vertical scrollbar that paints a small colored mark at each separator row,
// ported from MO2's ViewMarkingScrollBar. Marks are drawn only while the
// "Color the scrollbar at separators" setting is enabled.
class ModMarkingScrollBar : public QScrollBar {
    Q_OBJECT
public:
    explicit ModMarkingScrollBar(QTreeView* view);
    void set_model(QAbstractItemModel* model);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QTreeView* view_ = nullptr;
};

class ModTableView : public QTreeView {
    Q_OBJECT
public:
    explicit ModTableView(QWidget* parent = nullptr);

    // Re-apply the always-on / as-needed policy for the separator scrollbar
    // coloring setting (called after the Settings dialog closes).
    void apply_scrollbar_policy();

    void setModel(QAbstractItemModel* model) override;

signals:
    void files_dropped(const QStringList& paths);
    // Files/folders dragged out of the Overwrite info dialog onto mod row
    // `mod_row` (0-based into ModListModel). The view only reports drops it
    // recognized as living under the Overwrite directory.
    void overwrite_files_dropped(const QStringList& paths, int mod_row);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    bool is_under_overwrite(const QString& path) const;
};

}  // namespace ui
