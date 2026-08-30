#pragma once

#include <QHelpEvent>
#include <QIcon>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QScrollBar>
#include <QSize>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QStringList>

#include <algorithm>

class QAbstractItemModel;
class QAbstractItemView;
class QMouseEvent;

namespace ui {

class ModList;

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

constexpr int kFlagsSpacing = 2;  // px between adjacent flag icons

// Native cell size for a set of flag icons (all 16x16 today, but take the max
// so a larger icon anywhere in the list keeps every flag legible).
inline QSize flags_cell_size(const QList<QIcon>& icons) {
    QSize cell(16, 16);
    for (const auto& icon : icons)
        cell = cell.expandedTo(icon.actualSize(QSize(16, 16)));
    return cell;
}

// Height a `cell_width`-wide Flags cell needs for `icons`, growing by one icon
// line per wrap. Height 0 = no icons. FlagsDelegate::sizeHint and the plugins
// table (QTableWidget rows need explicit heights) both use this so the two
// layouts can never disagree.
inline QSize flags_wrapped_size(const QList<QIcon>& icons, int cell_width,
                                int spacing = kFlagsSpacing) {
    if (icons.isEmpty()) return QSize(0, 0);
    const QSize cell = flags_cell_size(icons);
    const int per_line = flags_icons_per_line(cell_width, cell.width(), spacing);
    const int lines = flags_icon_lines(icons.size(), per_line);
    return QSize(-1, lines * cell.height());
}

// Index of the flag icon under `local` (a point inside `cell_rect`) or -1.
// Shares the wrap math with FlagsDelegate::paint so the hover hit-test can
// never disagree with what is actually drawn.
inline int flag_icon_at(const QList<QIcon>& icons, const QRect& cell_rect,
                        const QPoint& local) {
    if (icons.isEmpty() || !cell_rect.contains(local)) return -1;
    const QSize cell = flags_cell_size(icons);
    const int per_line =
        flags_icons_per_line(cell_rect.width(), cell.width(), kFlagsSpacing);
    const int lines = flags_icon_lines(icons.size(), per_line);
    const int block_h = lines * cell.height();
    const int y0 = cell_rect.top() + std::max(0, (cell_rect.height() - block_h) / 2);
    const int step_x = cell.width() + kFlagsSpacing;
    const int col = (local.x() - cell_rect.left()) / step_x;
    const int row = (local.y() - y0) / cell.height();
    const int idx = row * per_line + col;
    if (idx < 0 || idx >= icons.size()) return -1;
    const QRect icon_rect(cell_rect.left() + (idx % per_line) * step_x,
                          y0 + (idx / per_line) * cell.height(),
                          cell.width(), cell.height());
    return icon_rect.contains(local) ? idx : -1;
}

// Delegate for the Flags column. The model returns each flag as an individual
// QIcon under `flag_icons_role`; this delegate paints them one by one at
// native size and, when the column is too narrow for all of them, wraps to
// extra lines and grows the row height (sizeHint). Qt's default single-
// DecorationRole rendering would have squeezed the stack down to one icon slot.
class FlagsDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // `flag_icons_role` is the model/user role carrying the QList<QIcon>.
    // When `flag_tooltips_role` is non-zero it carries a parallel QStringList
    // of per-icon hover text; helpEvent() shows ONLY the entry of the emblem
    // under the cursor (and nothing over empty cell space).
    explicit FlagsDelegate(int flag_icons_role, int flag_tooltips_role = 0,
                           QWidget* parent = nullptr);

protected:
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;

private:
    int flag_icons_role_ = Qt::UserRole;
    int flag_tooltips_role_ = 0;
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

// Delegate for the Name column: shifts the cell content right by the nesting
// depth (kIndentDepthRole) so nested mods read as indented under their parent.
// Purely visual - load order / priorities are untouched. The full-row
// background (selection / alternate tint) is drawn first on the unshifted
// rect, so the indentation gutter is never a gap in the row highlight; the
// checkbox + text + vendor glyph are then drawn shifted right.
class IndentDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit IndentDelegate(int indent_depth_role, QWidget* parent = nullptr);

protected:
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    int indent_depth_role_ = Qt::UserRole;
    static constexpr int kIndentStep = 20;  // px per nesting level (visible at depth 1)
};

class ModView : public QTreeView {
    Q_OBJECT
public:
    explicit ModView(QWidget* parent = nullptr);

    // Re-apply the always-on / as-needed policy for the separator scrollbar
    // coloring setting (called after the Settings dialog closes).
    void apply_scrollbar_policy();

    void setModel(QAbstractItemModel* model) override;
    // Re-layout rows whenever the Flags column is resized: the flag icons wrap
    // to extra lines (growing the row) based on the column width, and the
    // cached row heights must follow the drag. Hides the (non-virtual)
    // QTreeView::setHeader; main_window calls it through ModView*.
    void setHeader(QHeaderView* header);

signals:
    void files_dropped(const QStringList& paths);
    // Files/folders dragged out of the Overwrite info dialog onto mod row
    // `mod_row` (0-based into ModList). The view only reports drops it
    // recognized as living under the Overwrite directory.
    void overwrite_files_dropped(const QStringList& paths, int mod_row);
    // Ctrl+Double-Click on a mod row (MO2 parity): the controller opens the
    // OS file explorer at the mod's folder. Emitted only while the Ctrl
    // modifier is held; the plain double-click (Mod Info popup) is untouched.
    void ctrl_double_clicked(const QModelIndex& index);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    bool is_under_overwrite(const QString& path) const;
};

// Backward-compat alias
using ModTableView = ModView;

}  // namespace ui
