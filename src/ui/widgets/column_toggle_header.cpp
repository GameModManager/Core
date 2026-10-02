#include "ui/widgets/column_toggle_header.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QContextMenuEvent>
#include <QCursor>
#include <QHelpEvent>
#include <QMenu>
#include <QToolTip>
#include <QTreeView>

namespace ui {

void set_customizable_columns(QTreeView *view) {
  if (!view)
    return;
  // setHeader takes ownership and reparents the header to the view, so the
  // explicit parent argument is belt-and-braces only. No label list is passed:
  // the menu falls back to the model's headerData for each section, which is
  // what makes this usable without a hand-built label vector.
  auto *header = new ColumnToggleHeaderView(Qt::Horizontal, view);
  view->setHeader(header);
}

ColumnToggleHeaderView::ColumnToggleHeaderView(Qt::Orientation orientation,
                                               QWidget *parent)
    : QHeaderView(orientation, parent) {
  viewport()->installEventFilter(this);
}

void ColumnToggleHeaderView::set_column_labels(const QStringList &labels) {
  labels_ = labels;
}

void ColumnToggleHeaderView::note_user_visibility_choice() {
  user_visibility_choice_ = true;
}

void ColumnToggleHeaderView::set_section_tooltips(const QStringList &tooltips) {
  tooltips_ = tooltips;
}

QString ColumnToggleHeaderView::section_tooltip(int section) const {
  return tooltips_.value(section);
}

bool ColumnToggleHeaderView::section_is_enabled(int section) const {
  // QHeaderView is a QAbstractItemView, and a header handed to
  // QAbstractItemView::setHeader is given the view's model and keeps tracking
  // it across later model swaps - so the inherited model() is always the right
  // one, and unlike a stored raw pointer it cannot go stale or dangle.
  const QAbstractItemModel *model = this->model();
  if (!model)
    return true;
  const QVariant value = model->headerData(section, Qt::Horizontal, kEnabledColumnRole);
  // No value for the role means the model has no opinion, which MO2 reads as
  // "enabled" (uibase/widgetutility.h:11-14).
  if (!value.isValid())
    return true;
  return value.toBool();
}

// Text the context menu shows for `section`. Positional label list first, then
// the model's own header text, then the positional "Column N" fallback. The
// model is the step that matters for a caller that never passed labels at all
// (set_customizable_columns), so only a genuinely nameless section warns.
QString ColumnToggleHeaderView::section_label(int section) const {
  if (section < labels_.size())
    return labels_[section];
  if (model()) {
    const QString from_model =
        model()->headerData(section, Qt::Horizontal, Qt::DisplayRole).toString();
    if (!from_model.isEmpty())
      return from_model;
  }
  qWarning("ColumnToggleHeaderView: no label for section %d of %d sections", section,
           count());
  return tr("Column %1").arg(section + 1);
}

void ColumnToggleHeaderView::set_locked_section(int section) {
  if (!locked_sections_.contains(section))
    locked_sections_.append(section);
}

void ColumnToggleHeaderView::set_locked_sections(const QList<int> &sections) {
  locked_sections_ = sections;
}

bool ColumnToggleHeaderView::is_locked(int section) const {
  return locked_sections_.contains(section);
}

bool ColumnToggleHeaderView::eventFilter(QObject *obj, QEvent *event) {
  if (obj == viewport()) {
    if (event->type() == QEvent::ToolTip) {
      auto *he          = static_cast<QHelpEvent *>(event);
      const int section = logicalIndexAt(he->pos());
      const QString tip = section_tooltip(section);
      if (tip.isEmpty())
        QToolTip::hideText();
      else
        QToolTip::showText(he->globalPos(), tip, this);
      return true;
    }
    if (event->type() == QEvent::ContextMenu) {
      auto *cme = static_cast<QContextMenuEvent *>(event);
      QMenu menu(this);

      for (int i = 0; i < count(); ++i) {
        QAction *action = menu.addAction(section_label(i));
        action->setCheckable(true);
        if (is_locked(i)) {
          // Locked sections are always visible: the entry renders
          // checked + disabled so it reads "cannot be hidden".
          action->setChecked(true);
          action->setEnabled(false);
          continue;
        }
        action->setChecked(!isSectionHidden(i));
        if (!section_is_enabled(i)) {
          // The model says this column is not optional. Show its real state,
          // but refuse the toggle - unlike a locked section, which is always
          // forced visible.
          action->setEnabled(false);
          continue;
        }
        connect(action, &QAction::toggled, this, [this, i](bool checked) {
          const bool hidden = !checked;
          setSectionHidden(i, hidden);
          note_user_visibility_choice();
          emit section_toggled(i, hidden);
        });
      }

      menu.exec(cme->globalPos());
      return true;
    }
  }
  return QHeaderView::eventFilter(obj, event);
}

}  // namespace ui
