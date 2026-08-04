#pragma once

#include <QDialog>

class QKeyEvent;
class QLabel;
class QProgressBar;

namespace ui {

// MO2-style install progress popup. Shown modelessly with ApplicationModal
// window modality: the rest of the app is blocked (matching MO2's modal
// install), but the event loop keeps spinning so the worker's progress
// signals and the marshaled interactive install dialogs still arrive. No
// buttons - cancellation stays at the FOMOD wizard / overwrite steps, exactly
// like MO2. MainWindow shows it with a ~300ms delay so quick installs never
// flash it, hides it before each interactive install dialog, and closes it on
// install completion/cancel/failure.
class InstallProgressDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstallProgressDialog(QWidget* parent = nullptr);

    // Reset for the next install: window title + empty status + 0% bar.
    void begin(const QString& title);
    // Updates the status line and the bar. percent < 0 switches the bar to
    // the indeterminate (busy) animation; a determinate update switches back.
    void set_status(const QString& status, int percent);

protected:
    // No cancel: Escape must not dismiss the popup mid-install.
    void keyPressEvent(QKeyEvent* event) override;

private:
    QLabel* status_label_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
};

}  // namespace ui
