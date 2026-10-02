#pragma once

#include <QColor>
#include <QLabel>

namespace ui {

// A QLabel that lets a stylesheet define the app's link colour.
//
// QSS has no way to address QPalette::Link (it is not a qproperty), and
// QPalette::Link cannot be set per widget - every piece of link text in the
// process reads the application palette. So one LinkLabel on screen is enough
// to publish the colour:
//
//     LinkLabel { qproperty-linkColor: #3399FF; }
//
// Writing the property sets QPalette::Link and QPalette::LinkVisited on the
// application palette, so every widget that renders link text picks it up.
//
// MO2 parity: MOBase::LinkLabel (uibase/linklabel.h) - the same Q_PROPERTY and
// the same global-palette side effect, including MO2's limitation that a live
// QSS change does not recolour links already on screen (MO has to restart).
//
// Apart from that colour, LinkLabel is just a QLabel: the caller decides the
// text, and whether it is clickable, with setTextInteractionFlags() and
// setOpenExternalLinks().
//
// Note: Qt does not use QPalette::Link when rendering rich text (see
// QPalette::ColorRole), so a label that renders HTML needs its colour in the
// markup, not here.
class LinkLabel : public QLabel {
  Q_OBJECT
  Q_PROPERTY(QColor linkColor READ link_color WRITE set_link_color)

public:
  explicit LinkLabel(QWidget *parent = nullptr);

  // The colour currently published to the application palette.
  [[nodiscard]] QColor link_color() const;

  // Publish `color` as the application palette's Link and LinkVisited role, so
  // it survives a widget that later reads the palette. Applies to the
  // application palette immediately, which is the only place Qt looks for
  // link colours.
  void set_link_color(const QColor &color);

private:
  // Process-wide, exactly as MO2's is: the colour is a theme decision, not a
  // per-widget one, and Qt gives no per-widget link colour to store.
  static QColor link_color_;
};

}  // namespace ui