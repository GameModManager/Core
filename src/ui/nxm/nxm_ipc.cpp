#include "ui/nxm/nxm_ipc.h"
#include "engine/core/log/logger.h"
#include "platform/platform.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QDir>
#include <QCoreApplication>

#include <filesystem>

namespace {

std::string socket_path() {
    std::string base = engine::safe_home_dir().string() + "/.local/share/GameModManager";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base + "/gmm.sock";
}

}  // namespace

namespace engine {

// -- Server --

class NxmIpcServer::Impl {
public:
    QLocalServer server;
};

NxmIpcServer::NxmIpcServer(QObject* parent)
    : QObject(parent), impl_(new Impl) {
    connect(&impl_->server, &QLocalServer::newConnection, this, [this]() {
        auto* sock = impl_->server.nextPendingConnection();
        if (!sock) return;
        connect(sock, &QLocalSocket::readyRead, this, [this, sock]() {
            auto data = sock->readAll();
            auto url = QString::fromUtf8(data);
            if (!url.isEmpty()) {
                Logger::instance().debug("IPC: received nxm URL: " + url.toStdString());
                emit nxmUrlReceived(url);
            }
            sock->disconnectFromServer();
        });
    });
}

NxmIpcServer::~NxmIpcServer() {
    stopListening();
    delete impl_;
}

bool NxmIpcServer::startListening() {
    auto path = QString::fromStdString(socket_path());

    // Remove stale socket if no one is listening
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(500)) {
        // Another instance is already listening
        probe.disconnectFromServer();
        Logger::instance().warn("Another GMM instance is already running (IPC socket in use)");
        return false;
    }

    // No live listener, but a crashed session may have left a stale socket
    // file behind. QLocalServer::listen() will NOT remove it on Unix, so we
    // must do so explicitly (Qt docs: a crashed server leaves listen() failing
    // with AddressInUseError until the file is removed). Safe because the
    // single-instance guard guarantees we hold the lock.
    QLocalServer::removeServer(path);

    if (!impl_->server.listen(path)) {
        Logger::instance().warn("Failed to start IPC server (socket in use?): " +
            impl_->server.errorString().toStdString());
        return false;
    }

    return true;
}

void NxmIpcServer::stopListening() {
    if (impl_->server.isListening()) {
        auto path = impl_->server.serverName();
        impl_->server.close();
        QLocalServer::removeServer(path);
        Logger::instance().debug("IPC server stopped");
    }
}

bool NxmIpcServer::isListening() const {
    return impl_->server.isListening();
}

// -- Client --

bool send_nxm_to_running_instance(const QString& url) {
    auto path = QString::fromStdString(socket_path());

    QLocalSocket socket;
    socket.connectToServer(path);
    if (!socket.waitForConnected(200)) {
        Logger::instance().warn("No running GMM instance found (IPC connect failed)");
        return false;
    }

    socket.write(url.toUtf8());
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

}  // namespace engine
