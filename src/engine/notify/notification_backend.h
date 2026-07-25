#pragma once

#include <QObject>
#include <QString>

namespace engine {

class NotificationBackend : public QObject {
    Q_OBJECT
public:
    explicit NotificationBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~NotificationBackend() override = default;

    virtual void notify(const QString& title, const QString& message) = 0;
};

class InAppBackend : public NotificationBackend {
    Q_OBJECT
public:
    explicit InAppBackend(QObject* parent = nullptr) : NotificationBackend(parent) {}

    void notify(const QString& title, const QString& message) override;

signals:
    void notification_received(const QString& title, const QString& message);
};

}  // namespace engine
