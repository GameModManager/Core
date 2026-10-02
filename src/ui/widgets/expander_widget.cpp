#include "ui/widgets/expander_widget.h"

#include <QToolButton>

namespace ui {

namespace {

  // The arrow a button gets for each state. RightArrow closed (pointing at the
  // thing that would appear), DownArrow open.
  Qt::ArrowType arrow_for(bool opened) {
    return opened ? Qt::ArrowType::DownArrow : Qt::ArrowType::RightArrow;
  }

  constexpr int kStateSize = 1;

}  // namespace

ExpanderWidget::ExpanderWidget(QObject *parent) : QObject(parent) {}

ExpanderWidget::ExpanderWidget(QToolButton *button, QWidget *content, QObject *parent)
    : QObject(parent) {
  set(button, content);
}

void ExpanderWidget::set(QToolButton *button, QWidget *content, bool opened) {
  if (button_) {
    // Drop the old wiring first: re-pointing the expander at a new button must
    // not leave the previous one toggling it.
    button_->disconnect(this);
  }

  button_  = button;
  content_ = content;
  opened_  = opened;

  if (button_) {
    button_->setCheckable(true);
    button_->setChecked(opened_);
    connect(button_, &QToolButton::clicked, this, [this] {
      toggle(!opened_);
    });
  }
  apply();
}

void ExpanderWidget::toggle() {
  toggle(!opened_);
}

void ExpanderWidget::toggle(bool open) {
  if (open == opened_)
    return;  // nothing changed: no signal, no work
  emit about_to_toggle(open);
  opened_ = open;
  apply();
  emit toggled(open);
}

QByteArray ExpanderWidget::save_state() const {
  // One byte, 0 or 1. Deliberately not the raw bool: sizeof(bool) and its
  // representation are the compiler's business, and this blob is persisted.
  const char value = opened_ ? 1 : 0;
  return QByteArray(&value, kStateSize);
}

bool ExpanderWidget::restore_state(const QByteArray &state) {
  if (state.size() != kStateSize)
    return false;
  toggle(state.at(0) != 0);
  return true;
}

void ExpanderWidget::apply() {
  if (button_) {
    button_->setArrowType(arrow_for(opened_));
    button_->setChecked(opened_);
  }
  if (content_) {
    // A hidden widget stays hidden but keeps its place in the layout, which is
    // what a collapsible section wants - show/hide, not setParent.
    content_->setVisible(opened_);
  }
}

}  // namespace ui