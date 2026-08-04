#include "ui/install/install_name_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QVBoxLayout>

namespace ui {

QStringList InstallNameDialog::candidates(const QString& suggested_name,
                                          const QString& archive_filename) {
    QStringList names;
    auto push_unique = [&names](const QString& s) {
        if (!s.trimmed().isEmpty() && !names.contains(s)) names << s;
    };

    // 1. Best guess: the Nexus display name (or whatever the caller resolved).
    push_unique(suggested_name.trimmed());

    // 2. Archive-name best effort: strip the extension, drop the trailing
    //    Nexus download block ("-38604-5-2SE-1604800124"), turn '_' into
    //    spaces so "SkyUI_5_2_SE.zip" becomes the usable "SkyUI 5 2 SE".
    if (!archive_filename.isEmpty()) {
        QString stem = archive_filename;
        int dot = stem.lastIndexOf('.');
        if (dot > 0) stem = stem.left(dot);
        // Nexus-style "<name>_<version>-<modid>-<fileid>-<fileversion>-<ts>".
        // Only stripped when the tail actually matches the full block.
        static const QRegularExpression nexus_tail(
            R"(-[0-9]+-[0-9]+-[A-Za-z0-9._]+-[0-9]{8,}$)");
        stem.remove(nexus_tail);
        QString cleaned = stem.replace('_', ' ').simplified();
        push_unique(cleaned);

        // 3. The full archive filename is always offered as a fallback.
        push_unique(archive_filename);
    }

    if (names.isEmpty()) names << "New Mod";
    return names;
}

InstallNameDialog::InstallNameDialog(const QString& suggested_name,
                                     const QString& archive_filename,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Quick Install"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);

    auto* prompt = new QLabel(tr("Install as:"), this);
    layout->addWidget(prompt);

    name_combo_ = new QComboBox(this);
    name_combo_->setEditable(true);
    name_combo_->addItems(candidates(suggested_name, archive_filename));
    name_combo_->setCurrentIndex(0);
    layout->addWidget(name_combo_);

    auto* box =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    box->button(QDialogButtonBox::Ok)->setText(tr("Install"));
    box->button(QDialogButtonBox::Ok)->setDefault(true);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);
}

QString InstallNameDialog::name() const {
    if (!name_combo_) return {};
    return name_combo_->currentText().trimmed();
}

namespace {

std::optional<std::string> ask_install_name_impl(const std::string& suggested_name,
                                                 const std::string& archive_filename,
                                                 QWidget* parent) {
    InstallNameDialog dialog(QString::fromStdString(suggested_name),
                             QString::fromStdString(archive_filename), parent);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    QString name = dialog.name();
    if (name.isEmpty()) return std::nullopt;
    return name.toStdString();
}

}  // namespace

std::optional<std::string> ask_install_name(const std::string& suggested_name,
                                            const std::string& archive_filename,
                                            QWidget* parent) {
    if (QThread::currentThread() == qApp->thread()) {
        return ask_install_name_impl(suggested_name, archive_filename, parent);
    }
    // Marshal onto the main thread and block until the modal dialog is done.
    std::optional<std::string> result;
    QMetaObject::invokeMethod(
        qApp, [&] {
            result = ask_install_name_impl(suggested_name, archive_filename, parent);
        },
        Qt::BlockingQueuedConnection);
    return result;
}

}  // namespace ui
