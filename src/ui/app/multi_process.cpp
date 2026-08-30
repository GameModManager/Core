#include "ui/app/multi_process.h"
#include "engine/core/log/logger.h"
#include "platform/platform.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QDir>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace {

std::string lock_dir() {
    std::string base = engine::safe_home_dir().string() + "/.local/share/GameModManager";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

std::string lock_file_path() {
    return lock_dir() + "/gmm.lock";
}

std::string focus_socket_path() {
    return lock_dir() + "/gmm-focus.sock";
}

}  // namespace

namespace engine {

class MultiProcess::Impl {
public:
    QLockFile lock_file{QString::fromStdString(lock_file_path())};
    QLocalServer focus_server;
    bool locked = false;
};

MultiProcess::MultiProcess(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    connect(&impl_->focus_server, &QLocalServer::newConnection, this, [this]() {
        auto* sock = impl_->focus_server.nextPendingConnection();
        if (!sock) return;
        connect(sock, &QLocalSocket::readyRead, this, [this, sock]() {
            auto data = sock->readAll();
            if (data == "focus\n") {
                Logger::instance().debug("MultiProcess: focus request received");
                emit focusRequested();
            }
            sock->disconnectFromServer();
        });
    });
}

MultiProcess::~MultiProcess() {
    if (impl_->locked) {
        impl_->lock_file.unlock();
    }
    if (impl_->focus_server.isListening()) {
        auto path = impl_->focus_server.serverName();
        impl_->focus_server.close();
        QLocalServer::removeServer(path);
    }
}

bool MultiProcess::tryAcquire(int staleLockTimeoutMs) {
    if (impl_->locked) return true;

    // Try the lock. If it fails and the timeout > 0, QLockFile will attempt
    // to steal a stale lock (lock file whose PID no longer exists).
    if (!impl_->lock_file.tryLock(staleLockTimeoutMs)) {
        Logger::instance().debug("MultiProcess: another instance holds the lock");
        return false;
    }

    impl_->locked = true;

    // Remove any stale focus socket before starting the server
    auto path = QString::fromStdString(focus_socket_path());
    QLocalServer::removeServer(path);

    if (!impl_->focus_server.listen(path)) {
        Logger::instance().error("MultiProcess: failed to start focus server: " +
            impl_->focus_server.errorString().toStdString());
        // Even if the server fails, we still hold the lock - proceed
        return true;
    }

    Logger::instance().debug("MultiProcess: lock acquired, focus server started");
    return true;
}

bool MultiProcess::requestFocus() {
    auto path = QString::fromStdString(focus_socket_path());

    QLocalSocket socket;
    socket.connectToServer(path);
    if (!socket.waitForConnected(1000)) {
        Logger::instance().warn("MultiProcess: running instance not reachable");
        return false;
    }

    socket.write("focus\n");
    if (!socket.waitForBytesWritten(1000)) {
        Logger::instance().warn("MultiProcess: failed to send focus request");
        return false;
    }

    socket.disconnectFromServer();
    Logger::instance().debug("MultiProcess: focus request sent");
    return true;
}

}
