#include "ui/widgets/exec_entry_dialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ui {

// ---------------------------------------------------------------------------
// ExecEntry
// ---------------------------------------------------------------------------

QJsonObject ExecEntry::toJson() const {
    QJsonObject obj;
    obj["path"] = path;
    obj["title"] = title;
    obj["args"] = arguments;
    obj["cwd"]  = start_in;
    obj["mod"]  = output_mod;
    obj["icon"] = icon_path;
    return obj;
}

ExecEntry ExecEntry::fromJson(const QJsonObject& obj) {
    ExecEntry e;
    e.path       = obj["path"].toString();
    e.title      = obj["title"].toString();
    e.arguments  = obj["args"].toString();
    e.start_in   = obj["cwd"].toString();
    e.output_mod = obj["mod"].toString();
    e.icon_path  = obj["icon"].toString();
    return e;
}

ExecEntry ExecEntry::fromLegacyPath(const QString& relPath) {
    ExecEntry e;
    e.path = relPath;
    return e;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString ExecEntryDialog::display_name(const ExecEntry& e) {
    if (!e.title.isEmpty())
        return e.title;
    if (!e.path.isEmpty()) {
        auto last_slash = e.path.lastIndexOf('/');
        return last_slash >= 0 ? e.path.mid(last_slash + 1) : e.path;
    }
    return tr("Untitled");
}

// ---------------------------------------------------------------------------
// ExecEntryDialog
// ---------------------------------------------------------------------------

ExecEntryDialog::ExecEntryDialog(const std::filesystem::path& game_dir,
                                  const QVector<QPair<QString, QString>>& mod_list,
                                  const QVector<ExecEntry>& initial_entries,
                                  const std::filesystem::path& icon_cache_dir,
                                  QWidget* parent)
    : QDialog(parent), game_dir_(game_dir),
      icon_cache_dir_(icon_cache_dir),
      entries_(initial_entries) {
    setWindowTitle(tr("Modify Executables"));
    setMinimumSize(680, 460);

    auto* main = new QVBoxLayout(this);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // -- Left panel: entry list + Add/Remove buttons --
    auto* left_panel = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(0, 0, 0, 0);

    entry_list_ = new QListWidget(left_panel);
    entry_list_->setMinimumWidth(180);
    left_layout->addWidget(entry_list_);

    auto* btn_row = new QHBoxLayout;
    auto* add_btn = new QPushButton(tr("+ Add"), left_panel);
    auto* remove_btn = new QPushButton(tr("- Remove"), left_panel);
    btn_row->addWidget(add_btn);
    btn_row->addWidget(remove_btn);
    btn_row->addStretch();
    left_layout->addLayout(btn_row);

    splitter->addWidget(left_panel);

    // -- Right panel: form fields --
    auto* right_panel = new QWidget(this);
    auto* form = new QFormLayout(right_panel);
    form->setContentsMargins(8, 0, 0, 0);

    title_edit_ = new QLineEdit(right_panel);
    title_edit_->setPlaceholderText(tr("Leave empty to use the executable filename"));
    form->addRow(tr("Title:"), title_edit_);

    auto* binary_row = new QHBoxLayout;
    binary_edit_ = new QLineEdit(right_panel);
    binary_edit_->setPlaceholderText(tr("Relative path from game directory"));
    auto* browse_bin = new QPushButton(tr("Browse..."), right_panel);
    binary_row->addWidget(binary_edit_);
    binary_row->addWidget(browse_bin);
    form->addRow(tr("Binary:"), binary_row);

    args_edit_ = new QLineEdit(right_panel);
    args_edit_->setPlaceholderText(tr("Optional command-line arguments"));
    form->addRow(tr("Arguments:"), args_edit_);

    auto* cwd_row = new QHBoxLayout;
    start_in_edit_ = new QLineEdit(right_panel);
    start_in_edit_->setPlaceholderText(tr("Leave empty to use the game directory"));
    auto* browse_cwd = new QPushButton(tr("Browse..."), right_panel);
    cwd_row->addWidget(start_in_edit_);
    cwd_row->addWidget(browse_cwd);
    form->addRow(tr("Start in:"), cwd_row);

    output_mod_combo_ = new QComboBox(right_panel);
    output_mod_combo_->addItem(tr("--- None ---"), QVariant(""));
    for (const auto& [id, name] : mod_list) {
        output_mod_combo_->addItem(name, QVariant(id));
    }
    form->addRow(tr("Output to mod:"), output_mod_combo_);

    // Icon row: checkbox + preview + button
    auto* icon_row = new QHBoxLayout;
    use_app_icon_check_ = new QCheckBox(tr("Use Application's Icon for shortcuts"), right_panel);
    use_app_icon_check_->setChecked(true);
    icon_preview_ = new QLabel(right_panel);
    icon_preview_->setFixedSize(24, 24);
    icon_preview_->hide();
    change_icon_btn_ = new QPushButton(tr("Change icon"), right_panel);
    change_icon_btn_->setEnabled(false);
    icon_row->addWidget(use_app_icon_check_);
    icon_row->addWidget(icon_preview_);
    icon_row->addWidget(change_icon_btn_);
    icon_row->addStretch();
    form->addRow("", icon_row);

    right_panel->setMinimumWidth(300);
    splitter->addWidget(right_panel);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    main->addWidget(splitter);

    // Bottom buttons
    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, [this]() {
        if (validate())
            accept();
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    main->addWidget(buttons_);

    // -- Connections --
    connect(entry_list_, &QListWidget::currentRowChanged,
            this, &ExecEntryDialog::on_list_selection_changed);

    connect(add_btn, &QPushButton::clicked, this, &ExecEntryDialog::on_add_entry);
    connect(remove_btn, &QPushButton::clicked, this, &ExecEntryDialog::on_remove_entry);

    connect(title_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(binary_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(args_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(start_in_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(output_mod_combo_, &QComboBox::currentIndexChanged,
            this, &ExecEntryDialog::on_field_changed);

    connect(browse_bin, &QPushButton::clicked, this, &ExecEntryDialog::browse_binary);
    connect(browse_cwd, &QPushButton::clicked, this, &ExecEntryDialog::browse_start_in);
    connect(change_icon_btn_, &QPushButton::clicked, this, &ExecEntryDialog::on_change_icon);
    connect(use_app_icon_check_, &QCheckBox::toggled,
            this, &ExecEntryDialog::on_use_app_icon_toggled);

    // Populate list
    rebuild_list();
    if (!entries_.isEmpty())
        select_entry(0);
}

QVector<ExecEntry> ExecEntryDialog::entries() const {
    return entries_;
}

void ExecEntryDialog::rebuild_list() {
    QSignalBlocker blocker(entry_list_);
    entry_list_->clear();
    for (const auto& e : entries_) {
        entry_list_->addItem(display_name(e));
    }
}

void ExecEntryDialog::select_entry(int index) {
    if (index < 0 || index >= entries_.size()) {
        current_index_ = InvalidIndex;
        return;
    }

    save_current_entry();

    current_index_ = index;
    const auto& e = entries_[index];

    updating_fields_ = true;
    title_edit_->setText(e.title);
    binary_edit_->setText(e.path);
    args_edit_->setText(e.arguments);
    start_in_edit_->setText(e.start_in);
    int combo_idx = output_mod_combo_->findData(e.output_mod);
    output_mod_combo_->setCurrentIndex(combo_idx >= 0 ? combo_idx : 0);

    bool has_custom = !e.icon_path.isEmpty();
    use_app_icon_check_->setChecked(!has_custom);
    change_icon_btn_->setEnabled(has_custom);
    if (has_custom) {
        QPixmap pix(e.icon_path);
        if (!pix.isNull()) {
            icon_preview_->setPixmap(pix.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            icon_preview_->show();
        } else {
            icon_preview_->hide();
        }
    } else {
        icon_preview_->hide();
    }
    updating_fields_ = false;

    {
        QSignalBlocker blocker(entry_list_);
        entry_list_->setCurrentRow(index);
    }
}

void ExecEntryDialog::save_current_entry() {
    if (current_index_ < 0 || current_index_ >= entries_.size())
        return;

    auto& e = entries_[current_index_];
    e.title      = title_edit_->text().trimmed();
    e.path       = binary_edit_->text().trimmed();
    e.arguments  = args_edit_->text().trimmed();
    e.start_in   = start_in_edit_->text().trimmed();
    e.output_mod = output_mod_combo_->currentData().toString();
    if (use_app_icon_check_->isChecked())
        e.icon_path.clear();
    // icon_path unchanged when unchecked (already set via on_change_icon)
}

void ExecEntryDialog::on_add_entry() {
    save_current_entry();

    ExecEntry blank;
    entries_.append(blank);

    auto* item = new QListWidgetItem(display_name(blank), entry_list_);
    entry_list_->addItem(item);
    select_entry(entry_list_->count() - 1);
}

void ExecEntryDialog::on_remove_entry() {
    if (current_index_ < 0 || current_index_ >= entries_.size())
        return;

    save_current_entry();
    entries_.removeAt(current_index_);
    {
        QSignalBlocker blocker(entry_list_);
        delete entry_list_->takeItem(current_index_);
    }

    if (entries_.isEmpty()) {
        current_index_ = InvalidIndex;
        updating_fields_ = true;
        title_edit_->clear();
        binary_edit_->clear();
        args_edit_->clear();
        start_in_edit_->clear();
        output_mod_combo_->setCurrentIndex(0);
        use_app_icon_check_->setChecked(true);
        change_icon_btn_->setEnabled(false);
        icon_preview_->hide();
        updating_fields_ = false;
    } else {
        int next = std::min<int>(current_index_, entries_.size() - 1);
        select_entry(next);
    }
}

void ExecEntryDialog::on_change_icon() {
    if (current_index_ < 0 || current_index_ >= entries_.size())
        return;

    QString filter =
        tr("Icon files (*.ico *.png *.jpg *.jpeg *.svg);;"
           "Executables (*.exe);;"
           "All Files (*)");

    auto start_dir = game_dir_.empty()
        ? QDir::homePath()
        : QString::fromStdString(game_dir_.string());

    auto chosen = QFileDialog::getOpenFileName(this, tr("Select Icon"), start_dir, filter);
    if (chosen.isEmpty()) return;

    QString resolved = chosen;

    // For .exe files, try to extract the icon via wrestool
    if (QFileInfo(chosen).suffix().compare("exe", Qt::CaseInsensitive) == 0) {
        // Find wrestool
        auto app_dir = QCoreApplication::applicationDirPath();
        auto bundled = app_dir + "/../tools/linux/wrestool";
        QString wrestool;
        if (QFileInfo::exists(bundled))
            wrestool = bundled;
        else
            wrestool = QStandardPaths::findExecutable("wrestool");

        if (!wrestool.isEmpty()) {
            QTemporaryDir tmpDir;
            if (tmpDir.isValid()) {
                auto outIco = tmpDir.filePath("icon.ico");
                QProcess proc;
                proc.start(wrestool, {"-x", "-t", "14", chosen, "-o", outIco});
                if (proc.waitForFinished(3000) && proc.exitCode() == 0) {
                    // Cache the extracted icon
                    if (!icon_cache_dir_.empty()) {
                        auto cache_name = QFileInfo(chosen).completeBaseName() + ".ico";
                        auto cache_path = icon_cache_dir_ / cache_name.toStdString();
                        std::error_code ec;
                        std::filesystem::create_directories(icon_cache_dir_, ec);
                        if (!ec) {
                            std::filesystem::copy_file(
                                outIco.toStdString(), cache_path,
                                std::filesystem::copy_options::overwrite_existing, ec);
                            if (!ec)
                                resolved = QString::fromStdString(cache_path.string());
                        }
                    }
                    if (resolved == chosen)
                        resolved = outIco;
                }
            }
        }
    }

    auto& e = entries_[current_index_];
    e.icon_path = resolved;
    use_app_icon_check_->setChecked(false);
    change_icon_btn_->setEnabled(true);

    QPixmap pix(resolved);
    if (!pix.isNull()) {
        icon_preview_->setPixmap(pix.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        icon_preview_->show();
    }
}

void ExecEntryDialog::on_use_app_icon_toggled(bool checked) {
    if (updating_fields_) return;

    change_icon_btn_->setEnabled(!checked);
    if (checked) {
        icon_preview_->hide();
        if (current_index_ >= 0 && current_index_ < entries_.size()) {
            entries_[current_index_].icon_path.clear();
        }
    }
}

void ExecEntryDialog::on_list_selection_changed() {
    int row = entry_list_->currentRow();
    if (row == current_index_ || row < 0)
        return;
    select_entry(row);
}

void ExecEntryDialog::on_field_changed() {
    if (updating_fields_) return;

    if (current_index_ >= 0 && current_index_ < entries_.size()) {
        auto& e = entries_[current_index_];
        e.title      = title_edit_->text().trimmed();
        e.path       = binary_edit_->text().trimmed();
        e.arguments  = args_edit_->text().trimmed();
        e.start_in   = start_in_edit_->text().trimmed();
        e.output_mod = output_mod_combo_->currentData().toString();
        if (use_app_icon_check_->isChecked())
            e.icon_path.clear();

        auto* item = entry_list_->item(current_index_);
        if (item) {
            QSignalBlocker blocker(entry_list_);
            item->setText(display_name(e));
        }
    }
}

void ExecEntryDialog::browse_binary() {
    save_current_entry();

    auto start_dir = game_dir_.empty()
        ? QDir::homePath()
        : QString::fromStdString(game_dir_.string());

#ifdef Q_OS_WIN
    QString filter = tr("Executables (*.exe);;All Files (*)");
#else
    QString filter = tr("Executables (*.exe *.AppImage *.bin *.elf *.sh);;All Files (*)");
#endif

    auto path = QFileDialog::getOpenFileName(this, tr("Select Executable"), start_dir, filter);
    if (path.isEmpty()) return;

    QString rel;
    if (!game_dir_.empty()) {
        auto game_qdir = QDir(QString::fromStdString(game_dir_.string()));
        rel = game_qdir.relativeFilePath(path);
    } else {
        rel = path;
    }
    binary_edit_->setText(rel);

    if (title_edit_->text().trimmed().isEmpty())
        title_edit_->setText(QFileInfo(path).fileName());
}

void ExecEntryDialog::browse_start_in() {
    auto start_dir = start_in_edit_->text().isEmpty()
        ? (game_dir_.empty() ? QDir::homePath() : QString::fromStdString(game_dir_.string()))
        : start_in_edit_->text();

    auto dir = QFileDialog::getExistingDirectory(this, tr("Select Working Directory"), start_dir);
    if (!dir.isEmpty()) {
        start_in_edit_->setText(dir);
    }
}

bool ExecEntryDialog::validate() {
    save_current_entry();

    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].path.trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Modify Executables"),
                tr("Entry \"%1\" has no binary path set.\n"
                   "Please set a binary path or remove the entry.")
                    .arg(display_name(entries_[i])));
            select_entry(i);
            return false;
        }
    }
    return true;
}

}  // namespace ui
