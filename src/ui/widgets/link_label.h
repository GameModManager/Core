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
// application palette. That palette write IS the contract, and it is a narrow
// one: it is for code that reads QPalette::Link itself. Qt 6 does not draw any
// widget's link text from those roles - see the note at the bottom of this
// comment - so setting this property changes no pixels on its own.
//
// MO2 parity: MOBase::LinkLabel (uibase/linklabel.h) - the same Q_PROPERTY and
// the same global-palette side effect, including MO2's limitation that a live
// QSS change does not recolour links already on screen (MO has to restart).
//
// Apart from that colour, LinkLabel is just a QLabel: the caller decides the
// text, and whether it is clickable, with setTextInteractionFlags() and
// setOpenExternalLinks().
//
// Two Qt limits worth knowing before building a theme rule on top of this:
//   - QPalette::Link/LinkVisited are NOT used when Qt renders rich text (Qt
//     docs, QPalette::ColorRole: "we do not use the Link and LinkVisited roles
//     when rendering rich text in Qt"), which is the path a clickable QLabel
//     takes. Colour for a rich-text label has to live in the markup.
//   - QSS has no qproperty for the palette's Link role, which is the whole
//     reason this class exists in the first place.
// So: shipping this primitive creates the role and makes it themeable. Whether
// any given label visibly changes is a separate question, and Workspace-ylec
// carries that verification.
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