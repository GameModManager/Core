#include "ui/overwrite/query_overwrite_dialog.h"

#include "engine/pipeline/pipeline.h"

#include <QApplication>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QStyle>
#include <QThread>
#include <QVBoxLayout>

namespace ui {

QueryOverwriteDialog::QueryOverwriteDialog(const QString& mod_name,
                                           bool default_backup, QWidget* parent)
    : QDialog(parent), action_(engine::OverwriteAction::Cancel) {
    setWindowTitle(tr("Mod Exists"));
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setFrameShadow(QFrame::Raised);
    auto* frame_layout = new QHBoxLayout(frame);
    frame_layout->setContentsMargins(12, 12, 12, 12);
    frame_layout->setSpacing(12);

    auto* icon_label = new QLabel(frame);
    QIcon icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxQuestion);
    icon_label->setPixmap(icon.pixmap(128));
    frame_layout->addWidget(icon_label);

    auto* message = new QLabel(frame);
    message->setTextFormat(Qt::RichText);
    message->setWordWrap(true);
    message->setText(
        tr("This mod seems to be installed already, what would you like to do?<br><br>"
           "<b>Merge:</b> Add files from this archive overwriting existing ones.<br>"
           "<b>Replace:</b> Completely replace the existing mod (old files are deleted).<br>"
           "<b>Rename:</b> Install this as a separate mod with a new name <i>(recommended)</i>."));
    frame_layout->addWidget(message, 1);
    layout->addWidget(frame);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(7, 7, 7, 7);
    backup_box_ = new QCheckBox(tr("Keep Backup"), this);
    backup_box_->setChecked(default_backup);
    buttons->addWidget(backup_box_);
    buttons->addStretch(1);

    auto* merge_btn = new QPushButton(tr("Merge"), this);
    auto* replace_btn = new QPushButton(tr("Replace"), this);
    auto* rename_btn = new QPushButton(tr("Rename"), this);
    auto* cancel_btn = new QPushButton(tr("Cancel"), this);
    rename_btn->setDefault(true);
    for (auto* b : {merge_btn, replace_btn, rename_btn, cancel_btn})
        buttons->addWidget(b);
    layout->addLayout(buttons);

    connect(merge_btn, &QPushButton::clicked, this,
            [this] { set_action(engine::OverwriteAction::Merge); });
    connect(replace_btn, &QPushButton::clicked, this,
            [this] { set_action(engine::OverwriteAction::Replace); });
    connect(rename_btn, &QPushButton::clicked, this,
            [this] { set_action(engine::OverwriteAction::Rename); });
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
}

void QueryOverwriteDialog::set_action(engine::OverwriteAction action) {
    action_ = action;
    accept();
}

engine::OverwriteAction QueryOverwriteDialog::action() const { return action_; }

bool QueryOverwriteDialog::backup() const { return backup_box_->isChecked(); }

namespace {

engine::OverwriteDecision ask_overwrite_impl(const QString& mod_name,
                                             bool default_backup, QWidget* parent) {
    engine::OverwriteDecision decision;
    QueryOverwriteDialog dialog(mod_name, default_backup, parent);
    if (dialog.exec() != QDialog::Accepted ||
        dialog.action() == engine::OverwriteAction::Cancel) {
        return decision;  // action stays Cancel
    }
    decision.action = dialog.action();
    decision.backup = dialog.backup();

    // MO2's testOverwrite asks for the new folder name after the dialog
    // returns ACT_RENAME.
    if (decision.action == engine::OverwriteAction::Rename) {
        bool ok = false;
        QString new_name = QInputDialog::getText(
            parent, QObject::tr("Mod Name"), QObject::tr("Name"),
            QLineEdit::Normal, mod_name, &ok);
        if (!ok || new_name.trimmed().isEmpty()) {
            decision.action = engine::OverwriteAction::Cancel;
            return decision;
        }
        decision.new_name = new_name.toStdString();
    }
    return decision;
}

}  // namespace

engine::OverwriteDecision ask_overwrite(const QString& mod_name, bool default_backup,
                                        QWidget* parent) {
    if (QThread::currentThread() == qApp->thread()) {
        return ask_overwrite_impl(mod_name, default_backup, parent);
    }
    // Marshal onto the main thread and block until the modal dialog is done.
    // Same pattern as QtKeychainKeyring's run_on_main.
    engine::OverwriteDecision result;
    QMetaObject::invokeMethod(
        qApp, [&] { result = ask_overwrite_impl(mod_name, default_backup, parent); },
        Qt::BlockingQueuedConnection);
    return result;
}

}  // namespace ui
