#pragma once

#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QStringList>

#include <algorithm>

class QAbstractItemModel;

namespace ui {

class ModListModel;

// Flag-icon wrap math, shared by FlagsDelegate::paint and ::sizeHint so the
// two can never disagree and the logic is unit-testable without a widget.
// Number of icons that fit on one line of `cell_width` px (icon width +
// spacing between them), at least 1.
inline int flags_icons_per_line(int cell_width, int icon_width, int spacing) {
    const int step = icon_width + spacing;
    return step > 0 ? std::max(1, cell_width / step) : 1;
}
// Number of lines needed to lay out `count` icons at `per_line` per line.
inline int flags_icon_lines(int count, int per_line) {
    return per_line > 0 ? (count + per_line - 1) / per_line : 0;
}

// Delegate for the Flags column. The model returns each flag as an individual
// QIcon (ModListModel::kFlagIconsRole); this delegate paints them one by one at
// native size and, when the column is too narrow for all of them, wraps to
// extra lines and grows the row height (sizeHint). Qt's default single-
// DecorationRole rendering would have squeezed the stack down to one icon slot.
class FlagsDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
};

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
    // Re-layout rows whenever the Flags column is resized: the flag icons wrap
    // to extra lines (growing the row) based on the column width, and the
    // cached row heights must follow the drag. Hides the (non-virtual)
    // QTreeView::setHeader; main_window calls it through ModTableView*.
    void setHeader(QHeaderView* header);

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
