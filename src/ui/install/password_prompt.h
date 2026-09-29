#pragma once

#include <QString>

class QWidget;

namespace ui {

// Archive password prompt (MO2's InstallationManager::queryPassword port): a
// modal single-line dialog with the field in password mode, titled
// "Password required" so the install asks the same question MO2 does. GMM
// names the archive in the label as well - one install runs at a time, but a
// user with a dozen downloads open should not have to guess which one is
// blocked.
//
// Returns true with the password in `password` on accept. Returns false when
// the user closed the dialog or submitted nothing: that is a cancel, and the
// install is abandoned rather than failed.
//
// Safe to call from any thread: off the main thread it marshals the modal onto
// the main thread and waits (same pattern as ask_overwrite).
//
// The password lives in `password` and in the dialog's line edit, and nowhere
// else - it is not logged, not written to QSettings, and not kept for reuse.
bool ask_password(const QString &archive_name, QString &password,
                  QWidget *parent = nullptr);

}  // namespace ui
