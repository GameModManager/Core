#include "ui/widgets/exec_entry_dialog.h"

#include "engine/theme/icon_manager.h"

#include <QAbstractItemModel>
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
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include "ui/smooth_scroll.h"
#include "ui/settings/settings.h"

#include <filesystem>

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

QString exec_entry_display_name(const ExecEntry& e) {
    if (!e.title.isEmpty())
        return e.title;
    if (!e.path.isEmpty()) {
        auto last_slash = e.path.lastIndexOf('/');
        return last_slash >= 0 ? e.path.mid(last_slash + 1) : e.path;
    }
    return QStringLiteral("Untitled");
}

QString output_mod_for_path(const QVector<ExecEntry>& entries,
                            const std::filesystem::path& game_dir,
                            const QString& full_path) {
    if (game_dir.empty() || full_path.isEmpty())
        return {};

    // Canonicalize both sides (same rationale as browse_binary): the game dir
    // commonly goes through the ~/.steam symlink, so a raw comparison against
    // the symlinked spelling would dead-end.
    std::error_code ec;
    auto canon_base = std::filesystem::weakly_canonical(game_dir, ec);
    const auto base = ec || canon_base.empty() ? game_dir : canon_base;
    auto canon_full = std::filesystem::weakly_canonical(full_path.toStdString(), ec);
    if (ec || canon_full.empty())
        canon_full = full_path.toStdString();

    auto rel = std::filesystem::relative(canon_full, base, ec);
    if (ec || rel.empty())
        return {};
    // Paths escaping the game dir (e.g. /usr/bin/dolphin) produce a leading
    // ".."; an entry-path match is then impossible.
    if (rel.begin() != rel.end() && rel.begin()->string() == "..")
        return {};
    const QString rel_q = QString::fromStdString(rel.generic_string());
    if (rel_q.isEmpty())
        return {};
    const QString rel_lower = rel_q.toLower();

    for (const auto& e : entries) {
        if (!e.output_mod.isEmpty() && e.path.toLower() == rel_lower)
            return e.output_mod;
    }
    return {};
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

    // -- Left panel: entry list + Add/Remove/Up/Down toolbar --
    auto* left_panel = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left_panel);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(3);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(0);
    toolbar->addWidget(new QLabel(tr("Executables"), left_panel));
    toolbar->addStretch();

    auto make_btn = [this](const QString& tooltip, const QString& theme_icon,
                           QStyle::StandardPixmap fallback) {
        auto* btn = new QToolButton(this);
        btn->setToolTip(tooltip);
        btn->setIcon(engine::IconManager::instance().resolve_icon(theme_icon, fallback));
        btn->setAutoRaise(true);
        btn->setIconSize(QSize(20, 20));
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        return btn;
    };

    add_btn_ = make_btn(tr("Add an executable"), QStringLiteral("list-add"),
                        QStyle::SP_FileDialogNewFolder);
    remove_btn_ = make_btn(tr("Remove the selected executable"), QStringLiteral("list-remove"),
                           QStyle::SP_TrashIcon);
    up_btn_ = make_btn(tr("Move the executable up in the list"), QStringLiteral("go-up"),
                       QStyle::SP_ArrowUp);
    down_btn_ = make_btn(tr("Move the executable down in the list"), QStringLiteral("go-down"),
                         QStyle::SP_ArrowDown);

    auto* add_menu = new QMenu(add_btn_);
    add_menu->addAction(tr("Add from file..."), this, &ExecEntryDialog::on_add_from_file);
    add_menu->addAction(tr("Add empty"), this, &ExecEntryDialog::on_add_empty);
    add_menu->addAction(tr("Clone selected"), this, &ExecEntryDialog::on_clone_selected);
    add_btn_->setMenu(add_menu);
    add_btn_->setPopupMode(QToolButton::InstantPopup);

    toolbar->addWidget(add_btn_);
    toolbar->addWidget(remove_btn_);
    toolbar->addWidget(up_btn_);
    toolbar->addWidget(down_btn_);
    left_layout->addLayout(toolbar);

    entry_list_ = new QListWidget(left_panel);
    entry_list_->setMinimumWidth(180);
    entry_list_->setToolTip(tr("List of configured executables"));
    entry_list_->setDragDropMode(QAbstractItemView::InternalMove);
    entry_list_->setDefaultDropAction(Qt::MoveAction);
    left_layout->addWidget(entry_list_);

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
    output_mod_combo_->setEditable(true);
    output_mod_combo_->setInsertPolicy(QComboBox::NoInsert);
    output_mod_combo_->setToolTip(tr(
        "Type a mod name or pick one from the list. A name that is not in the\n"
        "list yet is auto-created as a new mod when the executable runs.\n"
        "Empty (--- None ---) routes output to Overwrite."));
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

    connect(remove_btn_, &QToolButton::clicked, this, &ExecEntryDialog::on_remove_entry);
    connect(up_btn_, &QToolButton::clicked, this, &ExecEntryDialog::on_up_clicked);
    connect(down_btn_, &QToolButton::clicked, this, &ExecEntryDialog::on_down_clicked);

    // Drag-drop reorder: the view already moved the selection before our slot
    // runs, so guard selection changes until we have rebuilt entries_.
    connect(entry_list_->model(), &QAbstractItemModel::rowsAboutToBeMoved,
            this, &ExecEntryDialog::on_rows_about_to_move);
    connect(entry_list_->model(), &QAbstractItemModel::rowsMoved,
            this, &ExecEntryDialog::on_rows_moved);

    connect(title_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(binary_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(args_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(start_in_edit_, &QLineEdit::textChanged, this, &ExecEntryDialog::on_field_changed);
    connect(output_mod_combo_, &QComboBox::currentIndexChanged,
            this, &ExecEntryDialog::on_field_changed);
    connect(output_mod_combo_, &QComboBox::editTextChanged,
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
    update_move_buttons();

    // TODO: gate behind a Settings "Smooth scrolling" checkbox.
    if (Settings::instance().smooth_scrolling())
        ui::enable_smooth_scrolling(this);
}

QVector<ExecEntry> ExecEntryDialog::entries() const {
    return entries_;
}

void ExecEntryDialog::rebuild_list() {
    QSignalBlocker blocker(entry_list_);
    entry_list_->clear();
    for (int i = 0; i < entries_.size(); ++i) {
        auto* item = new QListWidgetItem(exec_entry_display_name(entries_[i]));
        item->setData(Qt::UserRole, i);
        entry_list_->addItem(item);
    }
}

void ExecEntryDialog::restamp_list_indices() {
    for (int r = 0; r < entry_list_->count(); ++r)
        entry_list_->item(r)->setData(Qt::UserRole, r);
}

void ExecEntryDialog::select_entry(int index) {
    if (index < 0 || index >= entries_.size()) {
        current_index_ = InvalidIndex;
        update_move_buttons();
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
    if (combo_idx >= 0)
        output_mod_combo_->setCurrentIndex(combo_idx);
    else if (!e.output_mod.isEmpty())
        output_mod_combo_->setEditText(e.output_mod);
    else
        output_mod_combo_->setCurrentIndex(0);

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

    update_move_buttons();
}

void ExecEntryDialog::save_current_entry() {
    if (current_index_ < 0 || current_index_ >= entries_.size())
        return;

    auto& e = entries_[current_index_];
    e.title      = title_edit_->text().trimmed();
    e.path       = binary_edit_->text().trimmed();
    e.arguments  = args_edit_->text().trimmed();
    e.start_in   = start_in_edit_->text().trimmed();
    e.output_mod = current_output_mod_text();
    if (use_app_icon_check_->isChecked())
        e.icon_path.clear();
    // icon_path unchanged when unchecked (already set via on_change_icon)
}

QString ExecEntryDialog::current_output_mod_text() const {
    const QString text = output_mod_combo_->currentText().trimmed();
    // Selecting a listed mod returns its stored id; the "--- None ---" sentinel
    // returns its empty data. Anything else is free-typed and routed as-is.
    int idx = output_mod_combo_->findText(text);
    if (idx >= 0)
        return output_mod_combo_->itemData(idx).toString();
    return text;
}

void ExecEntryDialog::on_add_from_file() {
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

    ExecEntry e;
    // MO2 uses the binary's base name as the initial title.
    e.title = QFileInfo(path).completeBaseName();
    if (!game_dir_.empty()) {
        // Canonicalize both sides: game_dir commonly goes through the ~/.steam
        // symlink, so a raw relativeFilePath() produces a .. count that only
        // matches the realpath spelling and dead-ends at the symlinked one.
        std::error_code ec;
        auto canon_base = std::filesystem::weakly_canonical(game_dir_, ec);
        const auto base = ec || canon_base.empty() ? game_dir_ : canon_base;
        auto canon_path = QFileInfo(path).canonicalFilePath();
        if (canon_path.isEmpty()) canon_path = path;
        auto game_qdir = QDir(QString::fromStdString(base.string()));
        e.path = game_qdir.relativeFilePath(canon_path);
    } else {
        e.path = path;
    }
    add_new_entry(e);
}

void ExecEntryDialog::on_add_empty() {
    ExecEntry e;
    e.title = tr("New Executable");
    add_new_entry(e);
}

void ExecEntryDialog::on_clone_selected() {
    if (current_index_ < 0 || current_index_ >= entries_.size())
        return;
    save_current_entry();
    add_new_entry(entries_[current_index_]);
}

void ExecEntryDialog::add_new_entry(const ExecEntry& src) {
    ExecEntry e = src;
    e.title = make_non_conflicting_title(exec_entry_display_name(e));

    entries_.append(e);
    auto* item = new QListWidgetItem(exec_entry_display_name(e), entry_list_);
    item->setData(Qt::UserRole, entries_.size() - 1);
    entry_list_->addItem(item);
    select_entry(entry_list_->count() - 1);
    update_move_buttons();
}

QString ExecEntryDialog::make_non_conflicting_title(const QString& base) const {
    auto taken = [this](const QString& candidate) {
        for (const auto& e : entries_) {
            if (exec_entry_display_name(e) == candidate)
                return true;
        }
        return false;
    };

    if (!taken(base))
        return base;
    // MO2 pattern: "Name (1)", "Name (2)", ... bounded like ExecutablesList.
    for (int i = 1; i < 100; ++i) {
        auto candidate = QString("%1 (%2)").arg(base).arg(i);
        if (!taken(candidate))
            return candidate;
    }
    return base;
}

void ExecEntryDialog::on_remove_entry() {
    if (current_index_ < 0 || current_index_ >= entries_.size())
        return;

    save_current_entry();
    entries_.removeAt(current_index_);
    {
        QSignalBlocker blocker(entry_list_);
        delete entry_list_->takeItem(current_index_);
        restamp_list_indices();
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
        // Detach before selecting the replacement row: the implicit save inside
        // select_entry must not write the removed entry's form data into the
        // entry that slid into its slot.
        current_index_ = InvalidIndex;
        select_entry(next);
    }
    update_move_buttons();
}

void ExecEntryDialog::on_up_clicked() {
    if (current_index_ <= 0)
        return;
    move_entry(current_index_, current_index_ - 1);
}

void ExecEntryDialog::on_down_clicked() {
    if (current_index_ < 0 || current_index_ >= entries_.size() - 1)
        return;
    move_entry(current_index_, current_index_ + 1);
}

void ExecEntryDialog::move_entry(int from, int to) {
    if (from < 0 || from >= entries_.size() || to < 0 || to >= entries_.size())
        return;

    save_current_entry();
    entries_.move(from, to);

    {
        QSignalBlocker blocker(entry_list_);
        auto* item = entry_list_->takeItem(from);
        entry_list_->insertItem(to, item);
        restamp_list_indices();
    }

    // Point current_index_ at the moved entry first so the implicit save inside
    // select_entry writes back to the right slot (entries_ has been reordered).
    current_index_ = to;
    select_entry(to);
    update_move_buttons();
}

void ExecEntryDialog::on_rows_about_to_move(const QModelIndex&, int, int,
                                            const QModelIndex&, int) {
    // The view updates the selection during the move, before our rowsMoved
    // handler runs. Ignore those intermediate selection changes.
    reordering_ = true;
}

void ExecEntryDialog::on_rows_moved(const QModelIndex&, int, int,
                                    const QModelIndex&, int) {
    // Rebuild entries_ in the new list order. Each item still carries its
    // pre-move source index in UserRole.
    QVector<ExecEntry> reordered;
    reordered.reserve(entries_.size());
    bool consistent = (entry_list_->count() == entries_.size());
    for (int r = 0; consistent && r < entry_list_->count(); ++r) {
        int src = entry_list_->item(r)->data(Qt::UserRole).toInt();
        if (src < 0 || src >= entries_.size()) {
            consistent = false;
            break;
        }
        reordered.append(entries_[src]);
    }
    if (!consistent) {
        rebuild_list();
        reordering_ = false;
        update_move_buttons();
        return;
    }
    entries_ = reordered;

    // The dragged (selected) item now sits at the view's current row. Point
    // current_index_ there first so the implicit save in select_entry writes
    // back to the right entry.
    int row = entry_list_->currentRow();
    if (row < 0 || row >= entries_.size())
        row = entries_.isEmpty() ? InvalidIndex : 0;

    {
        QSignalBlocker blocker(entry_list_);
        restamp_list_indices();
    }

    current_index_ = row;
    if (row != InvalidIndex)
        select_entry(row);
    reordering_ = false;
    update_move_buttons();
}

void ExecEntryDialog::update_move_buttons() {
    up_btn_->setEnabled(current_index_ > 0);
    down_btn_->setEnabled(current_index_ >= 0 && current_index_ < entries_.size() - 1);
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
                    // Cache the extracted icon (keyed by full filename so the
                    // shared extractExeIcon cache lookup finds it)
                    if (!icon_cache_dir_.empty()) {
                        auto cache_name = QFileInfo(chosen).fileName() + ".ico";
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
    if (reordering_) return;

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
        e.output_mod = current_output_mod_text();
        if (use_app_icon_check_->isChecked())
            e.icon_path.clear();

        auto* item = entry_list_->item(current_index_);
        if (item) {
            QSignalBlocker blocker(entry_list_);
            item->setText(exec_entry_display_name(e));
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
        // Canonicalize both sides (same rationale as on_add_from_file): the
        // game dir commonly goes through the ~/.steam symlink.
        std::error_code ec;
        auto canon_base = std::filesystem::weakly_canonical(game_dir_, ec);
        const auto base = ec || canon_base.empty() ? game_dir_ : canon_base;
        auto canon_path = QFileInfo(path).canonicalFilePath();
        if (canon_path.isEmpty()) canon_path = path;
        auto game_qdir = QDir(QString::fromStdString(base.string()));
        rel = game_qdir.relativeFilePath(canon_path);
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
                    .arg(exec_entry_display_name(entries_[i])));
            select_entry(i);
            return false;
        }
    }
    return true;
}

}  // namespace ui
