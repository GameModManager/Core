#pragma once

namespace ui {

class MainWindow;

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
 *   locker.set_enabled(false);  // Lock UI
 *   // ... perform operation ...
 *   locker.set_enabled(true);   // Unlock UI
 */
class Locker {
public:
  explicit Locker(MainWindow *window);

  /**
   * @brief Enable or disable the UI
   * @param enabled true to enable, false to disable
   *
   * When disabled, the main window's central widget, menu bar, toolbar,
   * profile bar, and status bar are disabled. Modal dialogs remain
   * interactive.
   */
  void set_enabled(bool enabled);

private:
  MainWindow *w_;
};

} // namespace ui