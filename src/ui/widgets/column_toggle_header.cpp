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
  // Parented to the view, like every other hand-built header in the app, so it
  // dies with it (setHeader does not take ownership).
  auto *header = new ColumnToggleHeaderView(Qt::Horizontal, view);
  header->set_enablement_model(view->model());
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

void ColumnToggleHeaderView::set_enablement_model(QAbstractItemModel *model) {
  enablement_model_ = model;
}

bool ColumnToggleHeaderView::section_is_enabled(int section) const {
  if (!enablement_model_)
    return true;
  const QVariant value =
      enablement_model_->headerData(section, Qt::Horizontal, kEnabledColumnRole);
  // No value for the role means the model has no opinion, which MO2 reads as
  // "enabled" (uibase/widgetutility.h:11-14).
  if (!value.isValid())
    return true;
  return value.toBool();
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
        if (i >= labels_.size())
          // Positional list, and a short one ships a nameless entry: the
          // caller has to supply a label for every section the view has.
          qWarning("ColumnToggleHeaderView: no label for section %d of %d sections", i,
                   count());
        QString label = (i < labels_.size()) ? labels_[i] : tr("Column %1").arg(i + 1);
        QAction *action = menu.addAction(label);
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
