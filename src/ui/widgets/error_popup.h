#pragma once

#include <QString>
#include <functional>

class QWidget;

namespace ui {

class TaskDialog;

// Global error reporting surface (MO2 U268 port: uibase report.cpp
// reportError / criticalOnTop). One shared popup for criticalOnTop-style
// errors so call sites stop hand-rolling ad-hoc QMessageBox::critical /
// ::warning error flows:
//
//   ui::report_error(tr("Failed to terminate process group %1: %2")...);
//
// Behavior (MO2-exact, minus the Win32 pre-GUI branch which has no Linux
// equivalent):
//   - report_error logs the message (qCritical) and shows a modal
//     Critical-icon dialog titled "Error" with the message as the main
//     instruction and an optional collapsible details pane.
//   - critical_on_top does the same but raised (Qt::WindowStaysOnTopHint),
//     for errors raised with no main window around (MO2 criticalOnTop).
//
// Both are built on the reusable ui::TaskDialog, so the popup inherits the
// QPalette-first styling, the 3-tier text hierarchy, and the details
// expander for free.
//
// Relation to engine::InAppBackend: deliberately separate. InAppBackend is
// a non-modal fire-and-forget notification stream (no modality, no raise,
// no details, zero consumers today); report_error needs a modal raised
// critical popup. Different semantics, different component.

// Presents the fully-built error dialog. The default presenter runs it
// modally. Tests inject a stub to pin routing without showing UI; passing
// an empty presenter restores the default.
using ErrorPresenter = std::function<void(TaskDialog &)>;

void report_error(const QString &message, QWidget *parent = nullptr,
                  const QString &details = QString());
void critical_on_top(const QString &message, QWidget *parent = nullptr);
void set_error_presenter_for_tests(ErrorPresenter presenter);

}  // namespace ui
