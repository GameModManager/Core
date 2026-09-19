#include "ui/modpack/export_wizard.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "engine/gmmpack/packer.h"
#include "engine/mod/meta/mod_meta.h"

namespace ui {
namespace {

QScrollArea* wrap_scroll(QWidget* inner) {
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    return scroll;
}

bool is_separator_name(const std::string& name) {
    return name.find("_separator") != std::string::npos;
}

}  // namespace

ExportWizard::ExportWizard(const engine::InstanceSnapshot& snapshot,
                           std::filesystem::path mods_dir, QWidget* parent)
    : QDialog(parent), snapshot_(snapshot), mods_dir_(std::move(mods_dir)) {
    const QString pack_name = QString::fromStdString(snapshot_.display_name);
    setWindowTitle(pack_name.isEmpty()
                       ? tr("Export Modpack")
                       : tr("Export Modpack: %1").arg(pack_name));
    resize(900, 600);
    setMinimumSize(720, 480);

    build_steps();
    load_foreign_mods();
    build_mod_rows();
    build_exe_rows();

    auto* layout = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter);

    sidebar_ = new QListWidget(splitter);
    sidebar_->setMinimumWidth(140);
    sidebar_->setMaximumWidth(220);
    for (const auto& step : steps_) {
        auto* item = new QListWidgetItem(step.title, sidebar_);
        item->setData(Qt::UserRole, static_cast<int>(step.id));
    }
    splitter->addWidget(sidebar_);
    connect(sidebar_, &QListWidget::itemClicked, this,
            &ExportWizard::on_step_clicked);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(right);
    build_pages();
    right_layout->addWidget(stack_, 1);

    auto* nav = new QHBoxLayout();
    nav->addStretch(1);
    auto* cancel_button = new QPushButton(tr("Cancel"), right);
    back_button_ = new QPushButton(tr("< Back"), right);
    next_button_ = new QPushButton(tr("Next >"), right);
    next_button_->setDefault(true);
    nav->addWidget(cancel_button);
    nav->addWidget(back_button_);
    nav->addWidget(next_button_);
    right_layout->addLayout(nav);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 7);

    connect(back_button_, &QPushButton::clicked, this, &ExportWizard::on_back);
    connect(next_button_, &QPushButton::clicked, this, &ExportWizard::on_next);
    connect(cancel_button, &QPushButton::clicked, this,
            &ExportWizard::on_cancel);

    go_to(0);
    refresh_chrome();
}

QString ExportWizard::step_title(Step step) {
    switch (step) {
        case Step::Info: return tr("Info");
        case Step::Mods: return tr("Mods");
        case Step::Executables: return tr("Executables");
        case Step::Tree: return tr("Tree");
        case Step::Review: return tr("Review");
    }
    return {};
}

void ExportWizard::build_steps() {
    const std::vector<Step> order = {
        Step::Info, Step::Mods, Step::Executables, Step::Tree, Step::Review,
    };
    for (Step id : order) {
        StepState state;
        state.id = id;
        state.title = step_title(id);
        steps_.push_back(state);
    }
    steps_.front().visited = true;
}

// Snapshot folders referenced as someone's parent_separator are separators,
// not mods. Shared with the engine's own separator detection.
static std::unordered_set<std::string> separator_folders(
    const engine::InstanceSnapshot& snapshot) {
    std::unordered_set<std::string> separators;
    for (const auto& [folder, entry] : snapshot.mod_entries) {
        if (!entry.parent_separator.empty())
            separators.insert(entry.parent_separator);
    }
    return separators;
}

void ExportWizard::load_foreign_mods() {
    foreign_mods_.clear();
    if (snapshot_.profiles.empty()) return;
    for (const auto& entry : snapshot_.profiles.front().mods) {
        if (entry.foreign) foreign_mods_.insert(entry.mod_id);
    }
}

void ExportWizard::build_mod_rows() {
    // Collect mod folder names.  If mod_state.json exists (new instances)
    // we use the tracked entries; otherwise fall back to scanning the mods/
    // directory on disk so old instances still work.
    struct Row {
        int32_t pos = 0;
        std::string folder;
    };
    std::vector<Row> rows;

    if (!snapshot_.mod_entries.empty()) {
        const auto separators = separator_folders(snapshot_);
        for (const auto& [folder, entry] : snapshot_.mod_entries) {
            if (separators.count(folder) || is_separator_name(folder)) continue;
            const int32_t pos = entry.list_position < 0 ? INT32_MAX
                                                         : entry.list_position;
            rows.push_back({pos, folder});
        }
    } else if (!mods_dir_.empty() && std::filesystem::exists(mods_dir_)) {
        // Fallback: scan mods/ directory for folders that contain a meta.ini
        // or at minimum look like mod folders (skip hidden/system dirs and
        // separators, which are never exported as mods).
        for (const auto& entry : std::filesystem::directory_iterator(mods_dir_)) {
            if (!entry.is_directory()) continue;
            const std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (is_separator_name(name)) continue;
            rows.push_back({INT32_MAX, name});
            // Seed a synthetic tracking entry so filtered_snapshot() hands
            // the packer entries it can work with (old instances have an
            // empty mod_entries map).
            engine::ModTrackingEntry te;
            te.list_position = static_cast<int32_t>(rows.size());
            snapshot_.mod_entries[name] = te;
        }
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.pos != b.pos ? a.pos < b.pos : a.folder < b.folder;
    });

    for (const auto& row : rows) {
        ModRow out;
        out.folder = row.folder;
        const auto it = snapshot_.mod_entries.find(row.folder);
        out.disabled = it != snapshot_.mod_entries.end() && it->second.disabled;
        out.is_vanilla = foreign_mods_.count(row.folder) > 0;
        engine::ModMeta meta;
        if (!mods_dir_.empty()) meta = engine::ModMeta::load(mods_dir_, row.folder);
        out.source =
            QString::fromStdString(meta.source_type().empty() ? "manual"
                                                               : meta.source_type());
        out.resolvable = engine::gmmpack::resolve_mod_source(
                             meta, snapshot_.game_id, snapshot_.steam_appid)
                             .has_value();
        out.is_manual = !out.resolvable && !out.is_vanilla;
        // Vanilla masters can never be exported; manual/unknown sources are
        // opt-in (unchecked but selectable - they bundle as-is); disabled
        // mods are unchecked but still exportable if re-checked.
        if (out.is_vanilla) {
            out.included = false;
        } else if (out.is_manual) {
            out.included = false;
        } else {
            out.included = out.resolvable && !out.disabled;
        }
        mods_.push_back(std::move(out));
    }
}

void ExportWizard::build_exe_rows() {
    for (size_t i = 0; i < snapshot_.executables.size(); ++i) {
        const auto& exe = snapshot_.executables[i];
        ExeRow row;
        row.index = static_cast<int>(i);
        row.title = QString::fromStdString(
            exe.title.empty() ? exe.path : exe.title);
        row.path = QString::fromStdString(exe.path);
        row.args = QString::fromStdString(exe.args);
        exes_.push_back(std::move(row));
    }
}

void ExportWizard::build_pages() {
    stack_->addWidget(wrap_scroll(build_info_page()));         // Info
    stack_->addWidget(build_mods_page());                      // Mods
    stack_->addWidget(build_executables_page());               // Executables
    stack_->addWidget(build_tree_page());                      // Tree
    stack_->addWidget(wrap_scroll(build_review_page()));       // Review
}

void ExportWizard::go_to(int index) {
    if (index < 0 || index >= static_cast<int>(steps_.size())) return;
    if (steps_[index].skipped) return;
    current_ = index;
    steps_[current_].visited = true;
    stack_->setCurrentIndex(current_);
    on_page_entered(current_);
    refresh_chrome();
}

void ExportWizard::on_next() {
    if (steps_[current_].id == Step::Info &&
        name_edit_->text().trimmed().isEmpty()) {
        QMessageBox::information(this, tr("Info"),
                                 tr("Please give the modpack a name."));
        return;
    }
    if (steps_[current_].id == Step::Mods) {
        const bool any = std::any_of(mods_.begin(), mods_.end(),
                                     [](const ModRow& r) { return r.included; });
        if (!any) {
            QMessageBox::information(this, tr("Mods"),
                                     tr("Select at least one mod to export."));
            return;
        }
    }
    if (current_ >= static_cast<int>(steps_.size()) - 1) {
        on_export();
        return;
    }
    int next = current_ + 1;
    while (next < static_cast<int>(steps_.size()) && steps_[next].skipped) {
        ++next;
    }
    if (next >= static_cast<int>(steps_.size())) {
        on_export();
        return;
    }
    go_to(next);
}

void ExportWizard::on_back() {
    int prev = current_ - 1;
    while (prev >= 0 && steps_[prev].skipped) --prev;
    if (prev >= 0) go_to(prev);
}

void ExportWizard::on_cancel() {
    if (current_ > 0) {
        const auto answer = QMessageBox::question(
            this, tr("Export Modpack"),
            tr("Cancel the modpack export? Your choices will be lost."));
        if (answer != QMessageBox::Yes) return;
    }
    reject();
}

void ExportWizard::on_step_clicked(QListWidgetItem* item) {
    const int index = sidebar_->row(item);
    // Only back-navigation to already-visited steps; never forward past
    // the furthest visited step and never onto skipped steps.
    int furthest = current_;
    for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
        if (steps_[i].visited && !steps_[i].skipped) furthest = i;
    }
    if (index <= furthest && !steps_[index].skipped) go_to(index);
    refresh_chrome();
}

void ExportWizard::refresh_chrome() {
    const QPalette palette = this->palette();
    const QColor dim = palette.color(QPalette::Disabled, QPalette::WindowText);
    for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
        auto* item = sidebar_->item(i);
        QString label = steps_[i].title;
        QFont font = item->font();
        if (steps_[i].skipped) {
            font.setStrikeOut(true);
            font.setBold(false);
            item->setForeground(dim);
        } else if (i == current_) {
            font.setBold(true);
            font.setStrikeOut(false);
            label = QStringLiteral("\u25CF ") + label;
        } else if (steps_[i].visited) {
            font.setBold(false);
            font.setStrikeOut(false);
            label = QStringLiteral("\u2713 ") + label;
        } else {
            font.setBold(false);
            font.setStrikeOut(false);
            label = QStringLiteral("\u25CB ") + label;
        }
        item->setFont(font);
        item->setText(label);
    }
    sidebar_->setCurrentRow(current_);

    // Back hidden on the first step; Next becomes Export on Review.
    back_button_->setVisible(current_ != 0);
    const bool is_last = current_ == static_cast<int>(steps_.size()) - 1;
    next_button_->setText(is_last ? tr("Export") : tr("Next >"));
}

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------

QWidget* ExportWizard::build_info_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    auto* form = new QFormLayout();

    name_edit_ = new QLineEdit(page);
    name_edit_->setText(QString::fromStdString(snapshot_.display_name));
    form->addRow(tr("Pack name:"), name_edit_);

    author_edit_ = new QLineEdit(page);
    form->addRow(tr("Author:"), author_edit_);

    homepage_edit_ = new QLineEdit(page);
    homepage_edit_->setPlaceholderText(QStringLiteral("https://..."));
    form->addRow(tr("Homepage (optional):"), homepage_edit_);
    layout->addLayout(form);

    layout->addWidget(new QLabel(tr("Description:"), page));
    desc_edit_ = new QTextEdit(page);
    desc_edit_->setPlaceholderText(tr("What does this modpack do?"));
    layout->addWidget(desc_edit_);

    layout->addWidget(new QLabel(tr("Install instructions (optional):"), page));
    instructions_edit_ = new QTextEdit(page);
    layout->addWidget(instructions_edit_);

    layout->addStretch(1);
    return page;
}

QWidget* ExportWizard::build_mods_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    mods_count_ = new QLabel(page);
    layout->addWidget(mods_count_);

    auto* actions = new QHBoxLayout();
    actions->addStretch(1);
    auto* exclude = new QPushButton(tr("Exclude All Disabled"), page);
    connect(exclude, &QPushButton::clicked, this,
            &ExportWizard::on_exclude_disabled);
    actions->addWidget(exclude);
    layout->addLayout(actions);

    mods_table_ = new QTableWidget(page);
    mods_table_->setColumnCount(3);
    mods_table_->setHorizontalHeaderLabels({tr("Include"), tr("Name"),
                                            tr("Source")});
    mods_table_->horizontalHeader()->setStretchLastSection(true);
    mods_table_->horizontalHeader()->setSectionResizeMode(1,
                                                           QHeaderView::Stretch);
    mods_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    mods_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mods_table_->setRowCount(static_cast<int>(mods_.size()));

    const QPalette palette = this->palette();
    const QColor dim = palette.color(QPalette::Disabled, QPalette::WindowText);
    for (int row = 0; row < static_cast<int>(mods_.size()); ++row) {
        const ModRow& mod = mods_[row];
        auto* check = new QCheckBox(mods_table_);
        check->setChecked(mod.included);
        check->setProperty("row", row);
        if (mod.is_vanilla) {
            check->setChecked(false);
            check->setEnabled(false);
            check->setToolTip(
                tr("Vanilla game master - cannot be exported."));
        } else if (mod.is_manual) {
            check->setChecked(false);
            check->setEnabled(true);
            check->setToolTip(
                tr("Manual/unknown source. Checking this will bundle the "
                   "mod folder into the archive as-is."));
        } else if (mod.disabled) {
            check->setToolTip(tr("This mod is disabled in the instance."));
        }
        connect(check, &QCheckBox::toggled, this,
                &ExportWizard::on_mod_include_toggled);
        // Center the checkbox in its cell.
        auto* cell = new QWidget(mods_table_);
        auto* cell_layout = new QHBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setAlignment(Qt::AlignCenter);
        cell_layout->addWidget(check);
        mods_table_->setCellWidget(row, 0, cell);

        auto* name_item = new QTableWidgetItem(
            QString::fromStdString(mod.folder));
        name_item->setData(Qt::UserRole, row);
        if (mod.is_vanilla) {
            name_item->setForeground(dim);
            name_item->setToolTip(
                tr("Vanilla game master - cannot be exported."));
        } else if (mod.is_manual) {
            name_item->setToolTip(
                tr("Manual/unknown source. Checking this will bundle the "
                   "mod folder into the archive as-is."));
        } else if (mod.disabled) {
            name_item->setToolTip(tr("This mod is disabled in the instance."));
        }
        mods_table_->setItem(row, 1, name_item);
        auto* source_item = new QTableWidgetItem(mod.source);
        if (mod.is_vanilla || mod.is_manual) {
            source_item->setForeground(dim);
        }
        mods_table_->setItem(row, 2, source_item);
    }
    layout->addWidget(mods_table_, 1);

    refresh_mods_count();
    return page;
}

QWidget* ExportWizard::build_executables_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    if (exes_.empty()) {
        auto* note = new QLabel(tr("No executables in this instance."), page);
        note->setAlignment(Qt::AlignCenter);
        layout->addStretch(1);
        layout->addWidget(note);
        layout->addStretch(1);
        return page;
    }

    exes_table_ = new QTableWidget(page);
    exes_table_->setColumnCount(4);
    exes_table_->setHorizontalHeaderLabels(
        {tr("Include"), tr("Name"), tr("Path"), tr("Args")});
    exes_table_->horizontalHeader()->setStretchLastSection(true);
    exes_table_->horizontalHeader()->setSectionResizeMode(1,
                                                          QHeaderView::Stretch);
    exes_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    exes_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    exes_table_->setRowCount(static_cast<int>(exes_.size()));
    for (int row = 0; row < static_cast<int>(exes_.size()); ++row) {
        auto* check = new QCheckBox(exes_table_);
        check->setChecked(exes_[row].included);
        check->setProperty("row", row);
        connect(check, &QCheckBox::toggled, this,
                &ExportWizard::on_exe_include_toggled);
        auto* cell = new QWidget(exes_table_);
        auto* cell_layout = new QHBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setAlignment(Qt::AlignCenter);
        cell_layout->addWidget(check);
        exes_table_->setCellWidget(row, 0, cell);
        exes_table_->setItem(row, 1, new QTableWidgetItem(exes_[row].title));
        exes_table_->setItem(row, 2, new QTableWidgetItem(exes_[row].path));
        exes_table_->setItem(row, 3, new QTableWidgetItem(exes_[row].args));
    }
    layout->addWidget(exes_table_, 1);
    return page;
}

QWidget* ExportWizard::build_tree_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("Preview of the separator/mod layout written to the pack."), page));
    tree_ = new QTreeWidget(page);
    tree_->setHeaderLabel(tr("Modpack layout"));
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(tree_, 1);
    return page;
}

QWidget* ExportWizard::build_review_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    review_summary_ = new QLabel(page);
    review_summary_->setWordWrap(true);
    layout->addWidget(review_summary_);

    auto* form = new QFormLayout();
    output_edit_ = new QLineEdit(page);
    QString name = QString::fromStdString(snapshot_.display_name).trimmed();
    if (name.isEmpty()) name = tr("modpack");
    const QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString base = desktop.isEmpty() ? QStringLiteral("~") : desktop;
    output_edit_->setText(base + QStringLiteral("/") + name +
                          QStringLiteral(".gmmpack"));
    auto* row = new QWidget(page);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->addWidget(output_edit_, 1);
    auto* browse = new QPushButton(tr("Browse..."), row);
    connect(browse, &QPushButton::clicked, this,
            &ExportWizard::on_browse_output);
    row_layout->addWidget(browse);
    form->addRow(tr("Output file:"), row);
    layout->addLayout(form);

    progress_ = new QProgressBar(page);
    progress_->setMinimum(0);
    progress_->setMaximum(1);
    progress_->setValue(0);
    layout->addWidget(progress_);

    status_label_ = new QLabel(page);
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);

    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Page hooks + slots
// ---------------------------------------------------------------------------

void ExportWizard::on_page_entered(int index) {
    if (steps_[index].id == Step::Tree) {
        refresh_tree();
    } else if (steps_[index].id == Step::Review) {
        refresh_review();
    }
}

void ExportWizard::refresh_mods_count() {
    if (mods_count_ == nullptr) return;
    int selected = 0;
    for (const auto& mod : mods_) {
        if (mod.included) ++selected;
    }
    mods_count_->setText(tr("%1 of %2 mods selected")
                             .arg(selected)
                             .arg(static_cast<int>(mods_.size())));
}

void ExportWizard::refresh_tree() {
    if (tree_ == nullptr) return;
    tree_->clear();
    const QPalette palette = this->palette();
    const QColor dim = palette.color(QPalette::Disabled, QPalette::WindowText);
    const QIcon folder_icon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon mod_icon = style()->standardIcon(QStyle::SP_FileIcon);

    const engine::gmmpack::TreeRoot root =
        engine::gmmpack::build_tree(filtered_snapshot(), mods_dir_);
    std::function<void(QTreeWidgetItem*, const std::vector<engine::gmmpack::TreeNode>&)>
        add_nodes = [&](QTreeWidgetItem* parent,
                        const std::vector<engine::gmmpack::TreeNode>& nodes) {
            for (const auto& node : nodes) {
                if (std::holds_alternative<engine::gmmpack::SeparatorNode>(
                        node.data)) {
                    const auto& sep =
                        std::get<engine::gmmpack::SeparatorNode>(node.data);
                    auto* item = new QTreeWidgetItem(
                        {QString::fromStdString(sep.name)});
                    item->setIcon(0, folder_icon);
                    item->setExpanded(!sep.collapsed);
                    add_nodes(item, sep.children);
                    if (parent == nullptr) {
                        tree_->addTopLevelItem(item);
                    } else {
                        parent->addChild(item);
                    }
                } else {
                    const auto& mod =
                        std::get<engine::gmmpack::ModNode>(node.data);
                    QString label = QString::fromStdString(mod.id);
                    if (!mod.enabled) label += tr(" (disabled)");
                    auto* item = new QTreeWidgetItem({label});
                    item->setIcon(0, mod_icon);
                    if (!mod.enabled) item->setForeground(0, dim);
                    if (parent == nullptr) {
                        tree_->addTopLevelItem(item);
                    } else {
                        parent->addChild(item);
                    }
                }
            }
        };
    add_nodes(nullptr, root.nodes);
    tree_->expandAll();
}

void ExportWizard::refresh_review() {
    if (review_summary_ == nullptr) return;
    int mods = 0;
    for (const auto& mod : mods_) {
        if (!mod.included) continue;
        ++mods;
    }
    int exes = 0;
    for (const auto& exe : exes_) {
        if (exe.included) ++exes;
    }
    const QString name = name_edit_ ? name_edit_->text().trimmed()
                                    : QString::fromStdString(snapshot_.display_name);
    QString author = author_edit_ ? author_edit_->text().trimmed() : QString();
    if (author.isEmpty()) author = tr("Unknown");
    review_summary_->setText(
        tr("%1 by %2\nGame: %3\n%4 mods, %5 executables, %6 separators")
            .arg(name.isEmpty() ? tr("modpack") : name)
            .arg(author)
            .arg(QString::fromStdString(snapshot_.game_id))
            .arg(mods)
            .arg(exes)
            .arg(separator_count()));
}

engine::InstanceSnapshot ExportWizard::filtered_snapshot() const {
    engine::InstanceSnapshot out = snapshot_;
    const QString name =
        name_edit_ != nullptr ? name_edit_->text().trimmed() : QString();
    if (!name.isEmpty()) out.display_name = name.toStdString();
    for (const auto& row : mods_) {
        if (!row.included) out.mod_entries.erase(row.folder);
    }
    std::vector<engine::ExecutableEntry> kept;
    for (const auto& row : exes_) {
        if (row.included && row.index >= 0 &&
            row.index < static_cast<int>(snapshot_.executables.size())) {
            kept.push_back(snapshot_.executables[static_cast<size_t>(row.index)]);
        }
    }
    out.executables = std::move(kept);
    return out;
}

int ExportWizard::separator_count() const {
    return static_cast<int>(
        separator_folders(filtered_snapshot()).size());
}

void ExportWizard::on_exclude_disabled() {
    for (size_t i = 0; i < mods_.size(); ++i) {
        if (mods_[i].disabled && mods_[i].included) {
            mods_[i].included = false;
            if (mods_table_ != nullptr) {
                if (auto* cell = mods_table_->cellWidget(static_cast<int>(i), 0)) {
                    if (auto* check = cell->findChild<QCheckBox*>()) {
                        const bool blocked = check->blockSignals(true);
                        check->setChecked(false);
                        check->blockSignals(blocked);
                    }
                }
            }
        }
    }
    refresh_mods_count();
}

void ExportWizard::on_mod_include_toggled() {
    const auto* check = qobject_cast<const QCheckBox*>(sender());
    if (check == nullptr) return;
    const int row = check->property("row").toInt();
    if (row < 0 || row >= static_cast<int>(mods_.size())) return;
    mods_[static_cast<size_t>(row)].included = check->isChecked();
    refresh_mods_count();
}

void ExportWizard::on_exe_include_toggled() {
    const auto* check = qobject_cast<const QCheckBox*>(sender());
    if (check == nullptr) return;
    const int row = check->property("row").toInt();
    if (row < 0 || row >= static_cast<int>(exes_.size())) return;
    exes_[static_cast<size_t>(row)].included = check->isChecked();
}

void ExportWizard::on_browse_output() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Modpack"), output_edit_->text(),
        tr("Modpack (*.gmmpack)"));
    if (!path.isEmpty()) output_edit_->setText(path);
}

void ExportWizard::on_export() {
    const QString output =
        output_edit_ != nullptr ? output_edit_->text().trimmed() : QString();
    if (output.isEmpty()) {
        QMessageBox::information(this, tr("Review"),
                                 tr("Please choose an output file."));
        return;
    }
    engine::gmmpack::PackOptions options;
    options.author = author_edit_->text().toStdString();
    options.description = desc_edit_->toPlainText().toStdString();
    options.homepage = homepage_edit_->text().toStdString();
    options.instructions = instructions_edit_->toPlainText().toStdString();

    status_label_->setText(tr("Exporting..."));
    progress_->setRange(0, 0);  // busy; the packer runs synchronously
    const engine::gmmpack::PackResult result =
        engine::gmmpack::create_gmmpack(filtered_snapshot(), mods_dir_, options,
                                        std::filesystem::path(output.toStdString()));
    progress_->setRange(0, 1);
    progress_->setValue(result.ok ? 1 : 0);
    if (result.ok) {
        status_label_->setText(tr("Exported to %1").arg(output));
        QMessageBox::information(this, tr("Export Modpack"),
                                 tr("Modpack exported to %1").arg(output));
        accept();
    } else {
        status_label_->setText(
            tr("Export failed: %1")
                .arg(QString::fromStdString(result.error)));
        QMessageBox::warning(
            this, tr("Export Modpack"),
            tr("Export failed: %1").arg(QString::fromStdString(result.error)));
    }
}

}  // namespace ui
