#include "ui/widgets/link_label.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPalette>

namespace ui {

QColor LinkLabel::link_color_;

LinkLabel::LinkLabel(QWidget *parent) : QLabel(parent) {}

QColor LinkLabel::link_color() const {
  return link_color_;
}

void LinkLabel::set_link_color(const QColor &color) {
  link_color_ = color;

  auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!app)
    return;  // no QApplication yet: nothing to publish the colour to

  QPalette palette = app->palette();
  // LinkVisited as well as Link: MO2 has no distinct "visited" colour, and a
  // visited link that keeps the default magenta fights the theme.
  palette.setColor(QPalette::Link, color);
  palette.setColor(QPalette::LinkVisited, color);
  app->setPalette(palette);
}

}  // namespace ui