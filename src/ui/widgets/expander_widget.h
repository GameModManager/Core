#pragma once

#include <QByteArray>
#include <QObject>

class QToolButton;
class QWidget;

namespace ui {

// Pairs a QToolButton with the widget it reveals: the button gets an arrow that
// tracks the state, and clicking it shows or hides the content.
//
// A QObject rather than a widget, so it owns no layout - the button and the
// content can sit anywhere in the caller's tree (two columns, a dialog header,
// a filter section). MO2 parity: MOBase::ExpanderWidget
// (uibase/expanderwidget.h) has the same shape.
//
// No feature logic here: the caller supplies both widgets and decides what
// "expanded" means for its own layout.
//
// State transitions emit about_to_toggle before anything moves and toggled
// after, so a caller can veto-free measure the old layout before it changes.
// set() and a no-op toggle() (asked for the state it is already in) apply the
// state silently: no signal, because nothing changed.
class ExpanderWidget : public QObject {
  Q_OBJECT
public:
  // Empty expander; call set() before use.
  explicit ExpanderWidget(QObject *parent = nullptr);
  ExpanderWidget(QToolButton *button, QWidget *content, QObject *parent = nullptr);

  // Adopt `button` and `content`. The button is made checkable, gets an arrow,
  // and is wired to toggle the expander; `content` starts at `opened`
  // (closed by default). Calling set() again re-points the expander at a new
  // pair and drops the wiring on the old one.
  void set(QToolButton *button, QWidget *content, bool opened = false);

  // Flip the current state, or force it to `open`.
  void toggle();
  void toggle(bool open);

  [[nodiscard]] bool opened() const { return opened_; }
  [[nodiscard]] QToolButton *button() const { return button_; }
  [[nodiscard]] QWidget *content() const { return content_; }

  // Opaque 1-byte state blob, safe to round-trip through QSettings as a
  // QByteArray (MO2 persists it through GeometrySettings::saveState, see
  // references/modorganizer/src/settings.cpp:849).
  [[nodiscard]] QByteArray save_state() const;

  // Apply a blob from save_state(). Returns false and changes nothing when the
  // blob is empty or the wrong size, so a missing or stale setting is a no-op
  // rather than a wedge.
  bool restore_state(const QByteArray &state);

signals:
  // Emitted before the button arrow and the content visibility change.
  void about_to_toggle(bool opened);
  // Emitted after they have. `opened` is the new state.
  void toggled(bool opened);

private:
  void apply();

  QToolButton *button_ = nullptr;
  QWidget *content_    = nullptr;
  bool opened_         = false;
};

}  // namespace ui