#pragma once

#include <QObject>
#include <memory>

namespace engine {

// Ensures only one instance of the application runs at a time.
// Uses QLockFile for instance detection and QLocalServer/QLocalSocket for
// cross-process focus requests. Completely independent of NXM IPC.
class MultiProcess : public QObject {
  Q_OBJECT
public:
  explicit MultiProcess(QObject *parent = nullptr);
  ~MultiProcess() override;

  // Try to acquire the singleton lock.
  // Returns true if we are the sole instance (lock acquired, focus server started).
  // Returns false if another instance is already running.
  // Non-blocking by default (timeout 0): stale-lock detection (dead PID)
  // still runs, but we never wait on a live holder. A second instance
  // must probe and exit fast, not stall startup.
  bool tryAcquire(int staleLockTimeoutMs = 0);

  // Request the running instance to bring its window to the front.
  // Only meaningful when tryAcquire() returned false.
  bool requestFocus();

signals:
  // Emitted when another instance requests that we come to the front.
  void focusRequested();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace engine
