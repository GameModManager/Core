#include "ui/widgets/event_filter.h"

namespace ui {

EventFilter::EventFilter(QObject *parent, const HandlerFunc &handler)
    : QObject(parent), handler_(handler) {}

bool EventFilter::eventFilter(QObject *obj, QEvent *event) {
  if (!handler_)
    return QObject::eventFilter(obj, event);
  // A handler that only cares about one event type gets `false` for all the
  // others, which is the "not consumed" answer - so a handler can stay a plain
  // bool-returning lambda instead of having to fall through to the base class.
  return handler_(obj, event);
}

}  // namespace ui