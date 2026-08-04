#include "ui/install/install_progress_dialog.h"

#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>

namespace ui {

InstallProgressDialog::InstallProgressDialog(QWidget* parent)
    : QDialog(parent) {
    // ApplicationModal blocks input to every other window of the app (MO2's
    // modal install) while show() keeps the event loop running - required so
    // the worker's queued progress signals and the interactive install
    // dialogs' marshaled calls still reach this thread. Not exec()-modal: the
    // dialog is shown non-blockingly from MainWindow.
    setWindowModality(Qt::ApplicationModal);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setFixedWidth(420);

    auto* layout = new QVBoxLayout(this);

    status_label_ = new QLabel(tr("Installing…"), this);
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);

    progress_bar_ = new QProgressBar(this);
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(false);
    layout->addWidget(progress_bar_);
}

void InstallProgressDialog::begin(const QString& title) {
    setWindowTitle(title);
    status_label_->setText(QString());
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
}

void InstallProgressDialog::set_status(const QString& status, int percent) {
    if (!status.isEmpty()) status_label_->setText(status);
    if (percent < 0) {
        // Indeterminate stage (archive size unknowable): busy animation.
        progress_bar_->setRange(0, 0);
    } else {
        if (progress_bar_->maximum() == 0) progress_bar_->setRange(0, 100);
        progress_bar_->setValue(percent);
    }
}

void InstallProgressDialog::keyPressEvent(QKeyEvent* event) {
    // MO2 parity: the install progress popup has no cancel, so Escape must not
    // dismiss it. Cancellation lives at the FOMOD wizard / overwrite steps.
    if (event->key() == Qt::Key_Escape) {
        event->ignore();
        return;
    }
    QDialog::keyPressEvent(event);
}

}  // namespace ui
