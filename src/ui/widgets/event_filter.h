#pragma once

#include <QObject>
#include <functional>

class QEvent;

namespace ui {

// A generic event filter built from a callable. Install it on any QObject and
// the handler sees every event that object receives first: return true to
// consume the event (the target never sees it), false to let it through. That
// is the same contract as QObject::eventFilter, minus the subclass.
//
// MO2 parity: MOBase::EventFilter (uibase/eventfilter.h), which MO2 uses for
// exactly this - see references/modorganizer/src/mainwindow.cpp:375, where a
// one-line EventFilter stops wheel events from changing a combo the user is
// not focused on.
//
// The class is caller-supplied callback only: feature logic does not belong
// inside it. Parent the filter to an object whose lifetime covers the install,
// so the filter goes away with it.
class EventFilter : public QObject {
  Q_OBJECT
public:
  // Return true to consume the event, false to pass it on.
  using HandlerFunc = std::function<bool(QObject *, QEvent *)>;

  EventFilter(QObject *parent, const HandlerFunc &handler);

  // True when a handler is set. A default-constructed-ish filter with no
  // callable is inert: every event passes through.
  [[nodiscard]] bool has_handler() const { return static_cast<bool>(handler_); }

protected:
  bool eventFilter(QObject *obj, QEvent *event) override;

private:
  HandlerFunc handler_;
};

// Block the mouse wheel from changing ANY QComboBox selection in the process.
// Installed once on the QApplication object, and returned so a caller (a test,
// mostly) can take it back out.
//
// Application level rather than per widget on purpose: a filter installed on a
// QComboBox only ever sees that one combo, so a per-widget install misses
// every combo added later - which is how the settings, executables, instance
// options and dialog combos stayed unprotected while two of them were fixed.
// Qt runs the application filters inside QCoreApplication::notify, ahead of
// the receiver's own handler, so this lands before QComboBox::wheelEvent and
// before any style that overrides it.
//
// The wheel on an OPEN popup is deliberately left alone: the target there is
// the popup's viewport rather than the combo, so the cast below fails, the
// event passes, and the visible list scrolls without the selection moving
// underneath it. Only the closed-popup wheel - the accidental one - is stopped.
//
// Parent the returned filter to an object that outlives the install.
QObject *install_combo_wheel_guard(QObject *parent);

}  // namespace ui