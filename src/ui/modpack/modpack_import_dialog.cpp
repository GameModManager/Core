#include "ui/modpack/modpack_import_dialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>

#include "engine/gmmpack/unpacker.h"
#include "engine/pack/source_detector.h"

namespace ui {

namespace fs = std::filesystem;

ModpackImportDialog::ModpackImportDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Import Modpack"));
    setMinimumSize(520, 360);
    setSizeGripEnabled(true);
    setAcceptDrops(true);

    auto* layout = new QVBoxLayout(this);

    auto* file_card = new QGroupBox(tr("From file"), this);
    auto* file_layout = new QVBoxLayout(file_card);
    pick_button_ = new QPushButton(tr("Pick a .gmmpack archive..."), file_card);
    file_layout->addWidget(pick_button_);
    picked_file_label_ = new QLabel(tr("No file selected"), file_card);
    picked_file_label_->setWordWrap(true);
    file_layout->addWidget(picked_file_label_);
    auto* drop_hint = new QLabel(
        tr("Tip: you can also drag a .gmmpack file onto this dialog - "
           "or straight onto the main window."),
        file_card);
    drop_hint->setWordWrap(true);
    drop_hint->setEnabled(false);
    file_layout->addWidget(drop_hint);
    connect(pick_button_, &QPushButton::clicked, this,
            &ModpackImportDialog::on_pick_file);
    layout->addWidget(file_card);

    auto* or_label = new QLabel(tr("- or -"), this);
    or_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(or_label);

    auto* url_card = new QGroupBox(tr("From collection URL"), this);
    auto* url_layout = new QVBoxLayout(url_card);
    url_edit_ = new QLineEdit(url_card);
    url_edit_->setPlaceholderText(tr("Paste a collection URL..."));
    url_edit_->setClearButtonEnabled(true);
    url_layout->addWidget(url_edit_);
    connect(url_edit_, &QLineEdit::textChanged, this,
            &ModpackImportDialog::on_url_edited);
    layout->addWidget(url_card);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    import_button_ = box->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    import_button_->setDefault(true);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(import_button_, &QPushButton::clicked, this,
            &ModpackImportDialog::on_import);
    layout->addWidget(box);
}

void ModpackImportDialog::on_pick_file() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Modpack"), QString(),
        tr("Modpacks (*.gmmpack *.zip);;All files (*)"));
    if (path.isEmpty()) return;
    set_picked_file(path);
}

void ModpackImportDialog::set_picked_file(const QString& path) {
    if (path.isEmpty()) return;
    picked_file_ = path;
    picked_file_label_->setText(path);
    if (!url_edit_->text().trimmed().isEmpty()) url_edit_->clear();
}

void ModpackImportDialog::on_url_edited(const QString& text) {
    if (!text.trimmed().isEmpty() && !picked_file_.isEmpty()) {
        picked_file_.clear();
        picked_file_label_->setText(tr("No file selected"));
    }
}

fs::path ModpackImportDialog::resolve_schema_dir() {
    const fs::path app_dir =
        QCoreApplication::applicationDirPath().toStdString();
    // Installed layout first, then dev runs (walk up to Workspace/input/).
    std::vector<fs::path> candidates = {
        app_dir / "schemas",
        app_dir / ".." / "share" / "gamemodmanager" / "schemas",
    };
    std::error_code ec;
    for (fs::path dir = app_dir; !dir.empty(); dir = dir.parent_path()) {
        candidates.push_back(dir / "input");
        if (dir == dir.root_path()) break;
    }
    for (const auto& dir : candidates) {
        ec.clear();
        if (fs::exists(dir / "manifest.schema.json", ec)) return dir;
    }
    return {};
}

void ModpackImportDialog::on_import() {
    const QString url_text = url_edit_->text().trimmed();
    if (url_text.isEmpty() && picked_file_.isEmpty()) {
        QMessageBox::information(this, tr("Import Modpack"),
                                 tr("Pick a .gmmpack file or paste a "
                                    "collection URL to continue."));
        return;
    }

    if (!url_text.isEmpty()) {
        const QUrl url(url_text);
        if (!url.isValid() ||
            (url.scheme() != "http" && url.scheme() != "https")) {
            QMessageBox::warning(
                this, tr("Import Modpack"),
                tr("That doesn't look like a collection URL.\n"
                   "Expected an https:// link, got: %1")
                    .arg(url_text));
            return;
        }
    } else {
        const QFileInfo info(picked_file_);
        if (!info.exists()) {
            QMessageBox::warning(
                this, tr("Import Modpack"),
                tr("The selected file no longer exists:\n%1\n"
                   "Pick the file again.")
                    .arg(picked_file_));
            return;
        }
        if (!info.isFile() || !info.isReadable()) {
            QMessageBox::warning(
                this, tr("Import Modpack"),
                tr("Cannot read the selected file:\n%1").arg(picked_file_));
            return;
        }
    }

    const std::string ref = url_text.isEmpty()
                                ? picked_file_.toStdString()
                                : url_text.toStdString();

    const engine::Pack::Detection detection =
        engine::Pack::detect_pack_source(ref);
    if (!detection.known()) {
        QMessageBox::warning(this, tr("Import Modpack"),
                             tr("Unrecognized modpack source: %1")
                                 .arg(QString::fromStdString(detection.reason)));
        return;
    }

    if (detection.format == engine::Pack::PackFormat::NexusCollection) {
        QMessageBox::information(
            this, tr("Import Modpack"),
            tr("Collection URL import is not implemented yet. "
               "Please use a .gmmpack file for now."));
        return;
    }

    // .gmmpack path: unpack + validate.
    const fs::path schema_dir = resolve_schema_dir();
    if (schema_dir.empty()) {
        QMessageBox::warning(this, tr("Import Modpack"),
                             tr("Could not locate the gmmpack schemas "
                                "(manifest.schema.json)."));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const engine::gmmpack::UnpackResult result =
        engine::gmmpack::unpack_gmmpack(fs::path(ref), schema_dir);
    QApplication::restoreOverrideCursor();
    if (!result.ok) {
        QString errors;
        for (const auto& d : result.diagnostics) {
            if (d.severity == engine::gmmpack::Diagnostic::Severity::Error) {
                errors += QString::fromStdString(d.path + ": " + d.message + "\n");
            }
        }
        if (errors.isEmpty()) errors = tr("Unknown unpack error.");
        QMessageBox::warning(this, tr("Import Modpack"),
                             tr("Invalid modpack:\n%1").arg(errors));
        return;
    }
    pack_ = result.pack;
    accept();
}

void ModpackImportDialog::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString suffix =
            QFileInfo(url.toLocalFile()).suffix().toLower();
        if (suffix == "gmmpack" || suffix == "zip") {
            event->acceptProposedAction();
            return;
        }
    }
}

void ModpackImportDialog::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        const QString suffix = QFileInfo(local).suffix().toLower();
        if (!local.isEmpty() && (suffix == "gmmpack" || suffix == "zip")) {
            if (!QFileInfo::exists(local)) {
                QMessageBox::warning(
                    this, tr("Import Modpack"),
                    tr("The dropped file no longer exists:\n%1").arg(local));
                return;
            }
            set_picked_file(local);
            event->acceptProposedAction();
            return;
        }
    }
}

}  // namespace ui
