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
// Caller-supplied callback only: no feature logic belongs here. Parent the
// filter to an object whose lifetime covers the install, so the filter goes
// away with it.
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

}  // namespace ui