#include "ui/notify/notification_backend.h"

namespace engine {

void InAppBackend::notify(const QString& title, const QString& message) {
    emit notification_received(title, message);
}

}  // namespace engine
