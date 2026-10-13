#pragma once

#include <QPointer>
#include <QWidget>

#include <vector>

namespace ui {

class MainWindow;

// Why a lock is being taken. MO2's UILocker::Reasons has four; the two with no
// consumer here are absent on purpose. Its NoReason means "do not lock", which
// in GMM is simply not calling lock() at all - MO2 needs the enumerator
// because ProcessRunner threads the reason through a single postRun() path.
// Its PreventExit belongs to the exit-time wait for USVFS processes
// (processrunner.cpp:954), which GMM does not do.
enum class LockReason {
  // The plain lock, taken only while Settings::lock_gui() is on. Ticking it
  // off means the interface stays live during the operation, the way MO2's
  // unlock button behaves when clicked immediately.
  LockUI,
  // The operation's output has to be read back, so the lock is taken whether
  // or not the user turned locking off. MO2 overrides the setting for exactly
  // this case (processrunner.cpp:860-868): two concurrent installs are
  // impossible, so there is nothing for the user to race, and they are still
  // told what is happening.
  OutputRequired,
};

/**
 * @brief UI locker for disabling/enabling the main window during operations
 *
 * This class encapsulates the logic for locking and unlocking the UI
 * during long-running operations. It disables the main widget, menus,
 * toolbars, profile bar, and status bar while keeping modal dialogs
 * (FOMOD wizard, install progress, etc.) interactive.
 *
 * Usage:
 *   Locker locker(window);
 *   locker.lock(LockReason::OutputRequired);   // Lock UI
 *   // ... perform operation ...
 *   locker.unlock();                           // Unlock UI
 */
class Locker {
public:
  explicit Locker(MainWindow *window);

  /**
   * @brief Lock the UI, if the lock_gui setting allows it
   * @param reason why the lock is being taken
   * @return true when the interface was actually locked
   *
   * LockReason::OutputRequired ignores the setting; LockReason::LockUI is
   * gated on it. Nothing is disabled when the gate declines, and the return
   * value says so: only a caller whose lock actually engaged may call
   * unlock(), because there is one shared Locker per MainWindow and unlock()
   * is unconditional, so a caller unlocking a lock it never took would drop
   * somebody else's (an install started while a gated lock is declined).
   */
  bool lock(LockReason reason);

  /**
   * @brief Re-enable everything this locker disabled
   *
   * Unconditional: it restores the interface even when lock_gui is off, so a
   * setting change mid-operation can never strand a greyed-out window.
   */
  void unlock();

private:
  // MO2 uilocker.cpp:630. Only a widget that is currently enabled is
  // disabled and remembered, so unlock() never re-enables something the rest
  // of the app had already switched off.
  void disable(QWidget *w);

  MainWindow *w_;
  std::vector<QPointer<QWidget>> disabled_;
};

}  // namespace ui