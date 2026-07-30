#pragma once

#include <QObject>
#include <QString>

namespace engine {

// IPC server that listens for nxm:// URLs from other GMM processes.
// Uses QLocalServer (Unix domain socket on Linux, named pipe on Windows).
class NxmIpcServer : public QObject {
    Q_OBJECT
public:
    explicit NxmIpcServer(QObject* parent = nullptr);
    ~NxmIpcServer() override;

    // Start listening on a well-known socket path.
    // Returns false if another instance is already listening.
    bool startListening();

    // Stop listening and clean up the socket file.
    void stopListening();

    [[nodiscard]] bool isListening() const;

signals:
    // Emitted when another process sends an nxm:// URL.
    void nxmUrlReceived(const QString& url);

private:
    class Impl;
    Impl* impl_;
};

// Try to send an nxm:// URL to a running GMM instance.
// Returns true if the URL was delivered.
bool send_nxm_to_running_instance(const QString& url);

}  // namespace engine
