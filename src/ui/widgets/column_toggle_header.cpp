#include "ui/widgets/column_toggle_header.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QCursor>
#include <QHelpEvent>
#include <QMenu>
#include <QToolTip>

namespace ui {

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
          qWarning("ColumnToggleHeaderView: no label for section %d of %d sections",
                   i, count());
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
