// Offscreen GUI regression test for InstallProgressDialog - the MO2-style
// install progress popup. Exercises begin() reset, determinate vs
// indeterminate (busy) bar modes, the non-blocking ApplicationModal show (the
// event loop keeps running so the worker's queued progress signals still
// arrive), and that Escape cannot dismiss it (no cancel by design). Hermetic:
// no file access, QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/install/install_progress_dialog.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>

#include <cstdio>

static int failures = 0;
static int passes = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (cond)
        ++passes;
    else
        ++failures;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    ui::InstallProgressDialog dlg;
    auto* bar = dlg.findChild<QProgressBar*>();
    auto* label = dlg.findChild<QLabel*>();
    check(bar && label, "dialog has a progress bar and a status label");

    // ApplicationModal: blocks input to the rest of the app while the event
    // loop keeps spinning (progress signals still arrive) - not exec()-modal.
    check(dlg.windowModality() == Qt::ApplicationModal,
          "dialog is ApplicationModal (non-blocking show, blocking input)");

    if (!bar || !label) {
        std::printf("\n%d passed, %d failed\n", passes, failures);
        return 1;
    }

    // begin(): title + empty status + determinate 0%.
    dlg.begin(QStringLiteral("Installing…"));
    check(dlg.windowTitle() == QStringLiteral("Installing…"),
          "begin sets the window title");
    check(label->text().isEmpty(), "begin clears the status label");
    check(bar->maximum() == 100 && bar->value() == 0, "begin resets the bar to 0%");

    // Determinate updates drive the bar and the status line.
    dlg.set_status(QStringLiteral("Extracting mod.zip…"), 42);
    check(label->text() == QStringLiteral("Extracting mod.zip…"),
          "set_status updates the status line");
    check(bar->value() == 42, "set_status drives the bar");

    // Indeterminate stage (archive size unknowable) -> busy bar.
    dlg.set_status(QStringLiteral("Extracting mod.7z…"), -1);
    check(bar->maximum() == 0, "negative percent switches the bar to busy mode");

    // A determinate update switches back.
    dlg.set_status(QStringLiteral("Installing to SkyUI…"), 60);
    check(bar->maximum() == 100 && bar->value() == 60,
          "a determinate update switches the bar back to 0-100");

    // Non-blocking show: the dialog appears without entering a nested event
    // loop, so the worker's queued progress signals still reach the app.
    dlg.show();
    check(dlg.isVisible(), "show() displays the dialog non-blockingly");

    // No cancel: Escape must not dismiss the popup mid-install (MO2 parity).
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dlg, &esc);
    check(dlg.isVisible(), "Escape does not dismiss the install progress popup");

    dlg.hide();
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
