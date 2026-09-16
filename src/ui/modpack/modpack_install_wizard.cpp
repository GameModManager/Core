#include "ui/modpack/modpack_install_wizard.h"

#include <algorithm>
#include <variant>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace ui {
namespace {

using engine::gmmpack::Gmmpack;
using engine::gmmpack::ModCategory;
using engine::gmmpack::ModEntry;

QString source_provider(const ModEntry& mod) {
    return std::visit(
        [](const auto& src) -> QString {
            return QString::fromStdString(src.provider);
        },
        mod.source);
}

QString source_detail(const ModEntry& mod) {
    return std::visit(
        [](const auto& src) -> QString {
            using T = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<T, engine::gmmpack::ModSourceNexus>) {
                QString s = QString::fromStdString(src.game_domain) +
                            QStringLiteral(" / ") + QString::number(src.mod_id);
                if (src.file_name)
                    s += QStringLiteral(" (") +
                         QString::fromStdString(*src.file_name) +
                         QStringLiteral(")");
                return s;
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceDirect>) {
                return QString::fromStdString(src.url);
            } else if constexpr (std::is_same_v<
                                     T,
                                     engine::gmmpack::ModSourceSteamWorkshop>) {
                return QStringLiteral("app ") +
                       QString::number(src.app_id) + QStringLiteral(" item ") +
                       QString::number(src.workshop_item_id);
            } else {
                return QString::fromStdString(src.resolution);
            }
        },
        mod.source);
}

std::optional<int64_t> source_file_size(const ModEntry& mod) {
    return std::visit(
        [](const auto& src) -> std::optional<int64_t> {
            using T = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<T, engine::gmmpack::ModSourceNexus>) {
                return src.file_size;
            } else {
                return std::nullopt;
            }
        },
        mod.source);
}

QString format_size(std::optional<int64_t> bytes) {
    if (!bytes) return ModpackInstallWizard::tr("unknown size");
    const double b = static_cast<double>(*bytes);
    if (b < 1024.0) return QStringLiteral("%1 B").arg(*bytes);
    if (b < 1024.0 * 1024.0)
        return QStringLiteral("%1 KiB").arg(b / 1024.0, 0, 'f', 1);
    if (b < 1024.0 * 1024.0 * 1024.0)
        return QStringLiteral("%1 MiB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GiB")
        .arg(b / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString category_name(ModCategory category) {
    switch (category) {
        case ModCategory::Required: return ModpackInstallWizard::tr("Required");
        case ModCategory::Recommended:
            return ModpackInstallWizard::tr("Recommended");
        case ModCategory::Optional: return ModpackInstallWizard::tr("Optional");
    }
    return {};
}

const ModEntry* find_mod(const Gmmpack& pack, const std::string& id) {
    for (const auto& mod : pack.mods) {
        if (mod.id == id) return &mod;
    }
    return nullptr;
}

QString status_icon(const QString& status) {
    if (status == QStringLiteral("downloaded")) return QStringLiteral("\u2713");
    if (status == QStringLiteral("downloading"))
        return QStringLiteral("\u25C9");
    if (status == QStringLiteral("failed")) return QStringLiteral("\u2717");
    if (status == QStringLiteral("skipped")) return QStringLiteral("\u2013");
    return QStringLiteral("\u25CB");
}

QWidget* make_empty_note(const QString& text) {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addStretch(1);
    auto* label = new QLabel(text, page);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch(1);
    return page;
}

QScrollArea* wrap_scroll(QWidget* inner) {
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    return scroll;
}

}  // namespace

ModpackInstallWizard::ModpackInstallWizard(engine::gmmpack::Gmmpack pack,
                                           Mode mode, QWidget* parent)
    : QDialog(parent), pack_(std::move(pack)), mode_(mode) {
    const QString pack_name =
        QString::fromStdString(pack_.manifest.info.name);
    setWindowTitle(pack_name.isEmpty()
                       ? tr("Install Modpack")
                       : tr("Install Modpack: %1").arg(pack_name));
    resize(900, 600);
    setMinimumSize(720, 480);

    build_steps();

    // Seed persisted step state from pack defaults.
    for (const auto& group : pack_.manifest.choice_groups) {
        const QString gid = QString::fromStdString(group.id);
        if (group.mode == "exactly-one" && !group.member_mod_ids.empty()) {
            choice_selections_[gid] = QStringList(
                QString::fromStdString(group.member_mod_ids.front()));
        } else {
            choice_selections_[gid] = QStringList();
        }
    }
    for (const auto& entry : pack_.ini_edits) {
        for (const auto& tweak : entry.tweaks) {
            ini_enabled_[QString::fromStdString(tweak.id)] = tweak.enabled;
        }
    }
    for (const auto& mod : pack_.mods) {
        download_status_[QString::fromStdString(mod.id)] =
            QStringLiteral("pending");
    }
    for (const auto& patch : pack_.patches) {
        const QString key = QString::fromStdString(patch.mod_id) +
                            QStringLiteral("|") +
                            QString::fromStdString(patch.target_path);
        patch_allowed_[key] = false;  // Deny by default.
    }
    for (const auto& exe : pack_.executables) {
        if (exe.role == "setup" && exe.auto_run) {
            tool_status_[QString::fromStdString(exe.id)] =
                QStringLiteral("pending");
        }
    }

    auto* layout = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter);

    sidebar_ = new QListWidget(splitter);
    sidebar_->setMinimumWidth(140);
    sidebar_->setMaximumWidth(220);
    for (const auto& step : steps_) {
        auto* item = new QListWidgetItem(step.title, sidebar_);
        item->setData(Qt::UserRole, static_cast<int>(step.id));
        if (step.skipped) {
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable &
                           ~Qt::ItemIsEnabled);
        }
    }
    splitter->addWidget(sidebar_);
    connect(sidebar_, &QListWidget::itemClicked, this,
            &ModpackInstallWizard::on_step_clicked);

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

    connect(back_button_, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_back);
    connect(next_button_, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_next);
    connect(cancel_button, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_cancel);

    sim_timer_ = new QTimer(this);
    sim_timer_->setSingleShot(false);

    go_to(0);
    refresh_chrome();
}

QString ModpackInstallWizard::step_title(Step step) {
    switch (step) {
        case Step::Intro: return tr("Intro");
        case Step::Paths: return tr("Paths");
        case Step::Choices: return tr("Choices");
        case Step::IniTweaks: return tr("INI Tweaks");
        case Step::Downloads: return tr("Downloads");
        case Step::RunTools: return tr("Run Tools");
        case Step::Patches: return tr("Patches");
        case Step::Finishing: return tr("Finishing");
        case Step::End: return tr("End");
    }
    return {};
}

void ModpackInstallWizard::build_steps() {
    const std::vector<Step> order = {
        Step::Intro,   Step::Paths,   Step::Choices, Step::IniTweaks,
        Step::Downloads, Step::RunTools, Step::Patches, Step::Finishing,
        Step::End,
    };
    for (Step id : order) {
        StepState state;
        state.id = id;
        state.title = step_title(id);
        // Append mode reuses the existing instance: Paths already exist.
        state.skipped = (id == Step::Paths && mode_ == Mode::Append);
        state.visited = state.skipped;
        steps_.push_back(state);
    }
    // First visible step counts as visited.
    for (auto& step : steps_) {
        if (!step.skipped) {
            step.visited = true;
            break;
        }
    }
}

void ModpackInstallWizard::build_pages() {
    stack_->addWidget(wrap_scroll(build_intro_page()));      // Intro
    stack_->addWidget(wrap_scroll(build_paths_page()));      // Paths
    stack_->addWidget(wrap_scroll(build_choices_page()));    // Choices
    stack_->addWidget(wrap_scroll(build_ini_page()));        // INI Tweaks
    stack_->addWidget(build_downloads_page());               // Downloads
    stack_->addWidget(wrap_scroll(build_run_tools_page()));  // Run Tools
    stack_->addWidget(build_patches_page());                 // Patches
    stack_->addWidget(wrap_scroll(build_finishing_page()));  // Finishing
    stack_->addWidget(wrap_scroll(build_end_page()));        // End
}

void ModpackInstallWizard::go_to(int index) {
    if (index < 0 || index >= static_cast<int>(steps_.size())) return;
    if (steps_[index].skipped) return;
    current_ = index;
    steps_[current_].visited = true;
    stack_->setCurrentIndex(current_);
    on_page_entered(current_);
    refresh_chrome();
}

void ModpackInstallWizard::on_next() {
    if (current_ >= static_cast<int>(steps_.size()) - 1) {
        accept();
        return;
    }
    // Gate: every exactly-one choice group needs a selection.
    if (steps_[current_].id == Step::Choices && !choices_complete()) {
        QMessageBox::information(
            this, tr("Choices"),
            tr("Please pick one mod for each required choice group."));
        return;
    }
    // Gate: required downloads must finish first.
    if (steps_[current_].id == Step::Downloads && !downloads_complete()) {
        QMessageBox::information(
            this, tr("Downloads"),
            tr("Required mods are still downloading. "
               "Wait for them to finish or fix failures first."));
        return;
    }
    int next = current_ + 1;
    while (next < static_cast<int>(steps_.size()) && steps_[next].skipped) {
        ++next;
    }
    if (next >= static_cast<int>(steps_.size())) {
        accept();
        return;
    }
    go_to(next);
}

void ModpackInstallWizard::on_back() {
    int prev = current_ - 1;
    while (prev >= 0 && steps_[prev].skipped) --prev;
    if (prev >= 0) go_to(prev);
}

void ModpackInstallWizard::on_cancel() {
    if (current_ > 0) {
        const auto answer = QMessageBox::question(
            this, tr("Install Modpack"),
            tr("Cancel the modpack install? Progress will be lost."));
        if (answer != QMessageBox::Yes) return;
    }
    reject();
}

void ModpackInstallWizard::on_step_clicked(QListWidgetItem* item) {
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

void ModpackInstallWizard::refresh_chrome() {
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

    // Back hidden on the first visible step; Next becomes Finish on End.
    int first = 0;
    while (first < static_cast<int>(steps_.size()) && steps_[first].skipped) {
        ++first;
    }
    back_button_->setVisible(current_ != first);
    const bool is_last =
        current_ == static_cast<int>(steps_.size()) - 1;
    next_button_->setText(is_last ? tr("Finish") : tr("Next >"));
    refresh_next_enabled();
}

void ModpackInstallWizard::refresh_next_enabled() {
    if (steps_[current_].id == Step::Downloads) {
        next_button_->setEnabled(downloads_complete());
    } else {
        next_button_->setEnabled(true);
    }
}

bool ModpackInstallWizard::choices_complete() const {
    for (const auto& group : pack_.manifest.choice_groups) {
        if (group.mode != "exactly-one") continue;
        const QString gid = QString::fromStdString(group.id);
        if (choice_selections_.value(gid).isEmpty()) return false;
    }
    return true;
}

bool ModpackInstallWizard::downloads_complete() const {
    for (const auto& mod : pack_.mods) {
        if (mod.category != ModCategory::Required) continue;
        const QString status =
            download_status_.value(QString::fromStdString(mod.id));
        if (status != QStringLiteral("downloaded")) return false;
    }
    return true;
}

QString ModpackInstallWizard::mod_display_name(const std::string& id) const {
    const ModEntry* mod = find_mod(pack_, id);
    if (mod != nullptr && !mod->name.empty())
        return QString::fromStdString(mod->name);
    return QString::fromStdString(id);
}

QString ModpackInstallWizard::instance_root() const {
    return instance_root_edit_ ? instance_root_edit_->text() : QString();
}

QString ModpackInstallWizard::mods_dir() const {
    return mods_dir_edit_ ? mods_dir_edit_->text() : QString();
}

QString ModpackInstallWizard::downloads_dir() const {
    return downloads_dir_edit_ ? downloads_dir_edit_->text() : QString();
}

QString ModpackInstallWizard::profile_path() const {
    return profile_path_edit_ ? profile_path_edit_->text() : QString();
}

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------

QWidget* ModpackInstallWizard::build_intro_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    const auto& info = pack_.manifest.info;

    auto* name = new QLabel(
        QString::fromStdString(info.name.empty() ? pack_.manifest.id
                                                   : info.name),
        page);
    QFont title_font = name->font();
    title_font.setPointSize(title_font.pointSize() + 6);
    title_font.setBold(true);
    name->setFont(title_font);
    name->setWordWrap(true);
    layout->addWidget(name);

    if (!info.author.empty()) {
        layout->addWidget(new QLabel(tr("by %1").arg(
            QString::fromStdString(info.author)), page));
    }
    if (!info.description.empty()) {
        auto* desc = new QLabel(QString::fromStdString(info.description), page);
        desc->setWordWrap(true);
        layout->addWidget(desc);
    }
    if (!info.homepage.empty()) {
        auto* link = new QLabel(
            QStringLiteral("<a href=\"%1\">%1</a>")
                .arg(QString::fromStdString(info.homepage).toHtmlEscaped()),
            page);
        link->setOpenExternalLinks(true);
        link->setWordWrap(true);
        layout->addWidget(link);
    }

    QStringList dates;
    if (!info.created_at.empty())
        dates << tr("Created: %1").arg(
            QString::fromStdString(info.created_at));
    if (!info.updated_at.empty())
        dates << tr("Updated: %1").arg(
            QString::fromStdString(info.updated_at));
    if (!dates.isEmpty()) {
        auto* dates_label = new QLabel(dates.join(QStringLiteral("  |  ")),
                                       page);
        dates_label->setWordWrap(true);
        layout->addWidget(dates_label);
    }

    layout->addWidget(new QLabel(
        tr("%1 mods, revision %2")
            .arg(pack_.mods.size())
            .arg(pack_.manifest.revision),
        page));

    if (!pack_.manifest.tools.empty()) {
        auto* tools_box =
            new QGroupBox(tr("Required tools"), page);
        auto* tools_layout = new QVBoxLayout(tools_box);
        for (const auto& tool : pack_.manifest.tools) {
            QString entry = QString::fromStdString(
                tool.name.empty() ? tool.id : tool.name);
            if (!tool.homepage.empty()) {
                entry += QStringLiteral(" (<a href=\"%1\">%1</a>)")
                             .arg(QString::fromStdString(tool.homepage)
                                      .toHtmlEscaped());
            }
            auto* tool_label = new QLabel(entry, tools_box);
            tool_label->setOpenExternalLinks(true);
            tool_label->setWordWrap(true);
            tools_layout->addWidget(tool_label);
        }
        layout->addWidget(tools_box);
    }

    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_paths_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("Choose where the new instance and its directories live."), page));

    auto* form = new QFormLayout();
    instance_root_edit_ = new QLineEdit(page);
    mods_dir_edit_ = new QLineEdit(page);
    downloads_dir_edit_ = new QLineEdit(page);
    profile_path_edit_ = new QLineEdit(page);

    auto add_row = [&](const QString& label, QLineEdit* edit) {
        auto* row = new QWidget(page);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->addWidget(edit, 1);
        auto* browse = new QPushButton(tr("Browse..."), row);
        connect(browse, &QPushButton::clicked, this,
                [this, edit]() { on_browse_path(edit); });
        row_layout->addWidget(browse);
        form->addRow(label, row);
    };
    add_row(tr("Instance root:"), instance_root_edit_);
    add_row(tr("Mods directory:"), mods_dir_edit_);
    add_row(tr("Downloads directory:"), downloads_dir_edit_);
    add_row(tr("Profile path:"), profile_path_edit_);
    layout->addLayout(form);
    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_choices_page() {
    if (pack_.manifest.choice_groups.empty()) {
        return make_empty_note(tr("No choices to make."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    for (const auto& group : pack_.manifest.choice_groups) {
        const QString gid = QString::fromStdString(group.id);
        auto* box = new QGroupBox(
            QString::fromStdString(group.name.empty() ? group.id : group.name) +
                (group.mode == "exactly-one"
                     ? tr(" (pick one)")
                     : tr(" (pick at most one)")),
            page);
        auto* box_layout = new QVBoxLayout(box);
        const bool exactly_one = group.mode == "exactly-one";
        QButtonGroup* radio_group = nullptr;
        if (exactly_one) {
            radio_group = new QButtonGroup(box);
            radio_group->setExclusive(true);
        }
        for (const auto& member_id : group.member_mod_ids) {
            const QString mid = QString::fromStdString(member_id);
            const QString label = mod_display_name(member_id);
            const bool selected =
                choice_selections_.value(gid).contains(mid);
            if (exactly_one) {
                auto* radio = new QRadioButton(label, box);
                radio->setChecked(selected);
                radio->setProperty("group_id", gid);
                radio->setProperty("mod_id", mid);
                radio_group->addButton(radio);
                connect(radio, &QRadioButton::toggled, this,
                        &ModpackInstallWizard::on_choice_toggled);
                box_layout->addWidget(radio);
            } else {
                auto* check = new QCheckBox(label, box);
                check->setChecked(selected);
                check->setProperty("group_id", gid);
                check->setProperty("mod_id", mid);
                connect(check, &QCheckBox::toggled, this,
                        &ModpackInstallWizard::on_choice_toggled);
                box_layout->addWidget(check);
            }
        }
        layout->addWidget(box);
    }
    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_ini_page() {
    if (pack_.ini_edits.empty()) {
        return make_empty_note(tr("No INI tweaks to configure."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    for (const auto& entry : pack_.ini_edits) {
        auto* box = new QGroupBox(
            QString::fromStdString(entry.target_file), page);
        auto* box_layout = new QVBoxLayout(box);
        for (const auto& tweak : entry.tweaks) {
            const QString tid = QString::fromStdString(tweak.id);
            const bool required = tweak.status == "required";
            QString label = QString::fromStdString(tweak.name);
            label += required ? tr(" (required)") : tr(" (recommended)");
            if (tweak.has_source_mod_id && !tweak.source_mod_id.empty()) {
                label += tr(" [from %1]").arg(
                    mod_display_name(tweak.source_mod_id));
            }
            auto* check = new QCheckBox(label, box);
            check->setProperty("tweak_id", tid);
            if (required) {
                check->setChecked(true);
                check->setEnabled(false);
                ini_enabled_[tid] = true;
            } else {
                check->setChecked(ini_enabled_.value(tid, tweak.enabled));
                connect(check, &QCheckBox::toggled, this,
                        &ModpackInstallWizard::on_ini_toggled);
            }
            box_layout->addWidget(check);
        }
        layout->addWidget(box);
    }
    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_downloads_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    downloads_bar_ = new QProgressBar(page);
    downloads_bar_->setMinimum(0);
    downloads_bar_->setMaximum(100);
    layout->addWidget(downloads_bar_);

    downloads_table_ = new QTableWidget(page);
    downloads_table_->setColumnCount(5);
    downloads_table_->setHorizontalHeaderLabels(
        {tr(""), tr("Mod"), tr("Source"), tr("Size"), tr("")});
    downloads_table_->horizontalHeader()->setStretchLastSection(true);
    downloads_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    downloads_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloads_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(downloads_table_, 1);

    auto* hint = new QLabel(
        tr("Nexus downloads use the API when premium; otherwise the "
           "browser opens. Next unlocks once all required mods finish."),
        page);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    refresh_downloads_ui();
    return page;
}

QWidget* ModpackInstallWizard::build_run_tools_page() {
    std::vector<const engine::gmmpack::ExecutableEntry*> setup;
    for (const auto& exe : pack_.executables) {
        if (exe.role == "setup" && exe.auto_run) setup.push_back(&exe);
    }
    if (setup.empty()) {
        return make_empty_note(tr("No setup tools to run."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    tools_status_ = new QLabel(page);
    tools_status_->setWordWrap(true);
    layout->addWidget(tools_status_);

    tools_table_ = new QTableWidget(page);
    tools_table_->setColumnCount(4);
    tools_table_->setHorizontalHeaderLabels(
        {tr("Tool"), tr("Source mod"), tr("Status"), tr("")});
    tools_table_->horizontalHeader()->setStretchLastSection(true);
    tools_table_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    tools_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tools_table_->setRowCount(static_cast<int>(setup.size()));
    int row = 0;
    for (const auto* exe : setup) {
        const QString eid = QString::fromStdString(exe->id);
        tools_table_->setItem(
            row, 0, new QTableWidgetItem(QString::fromStdString(exe->id)));
        tools_table_->setItem(row, 1,
                              new QTableWidgetItem(mod_display_name(
                                  exe->source_mod_id)));
        tools_table_->setItem(
            row, 2, new QTableWidgetItem(tool_status_.value(eid)));
        tools_table_->item(row, 0)->setData(Qt::UserRole, eid);
        auto* run = new QPushButton(tr("Run"), tools_table_);
        run->setProperty("exe_id", eid);
        connect(run, &QPushButton::clicked, this,
                &ModpackInstallWizard::on_run_tool_one);
        tools_table_->setCellWidget(row, 3, run);
        ++row;
    }
    layout->addWidget(tools_table_, 1);

    // Surface output-capture and argument details below the table.
    QStringList notes;
    for (const auto* exe : setup) {
        QString note = QString::fromStdString(exe->id) + QStringLiteral(": ") +
                       QString::fromStdString(exe->relative_path);
        if (!exe->arguments.empty()) {
            QStringList args;
            for (const auto& arg : exe->arguments)
                args << QString::fromStdString(arg);
            note += QStringLiteral(" ") + args.join(QStringLiteral(" "));
        }
        if (exe->output && exe->output->capture == "syntheticMod") {
            note += tr(" (output captured as mod %1)")
                        .arg(QString::fromStdString(
                            exe->output->synthetic_mod_id));
        }
        notes << note;
    }
    auto* detail = new QLabel(notes.join(QStringLiteral("\n")), page);
    detail->setWordWrap(true);
    layout->addWidget(detail);
    tools_status_->setText(tr("Setup tools are run in pack order."));
    return page;
}

QWidget* ModpackInstallWizard::build_patches_page() {
    if (pack_.patches.empty()) {
        return make_empty_note(tr("No binary patches."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("These patches modify mod files. Allow only patches you trust."),
        page));

    patches_table_ = new QTableWidget(page);
    patches_table_->setColumnCount(4);
    patches_table_->setHorizontalHeaderLabels(
        {tr("Mod"), tr("Patch"), tr("Status"), tr("File")});
    patches_table_->horizontalHeader()->setStretchLastSection(true);
    patches_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    patches_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    patches_table_->setRowCount(static_cast<int>(pack_.patches.size()));
    int row = 0;
    for (const auto& patch : pack_.patches) {
        const QString key = QString::fromStdString(patch.mod_id) +
                            QStringLiteral("|") +
                            QString::fromStdString(patch.target_path);
        patches_table_->setItem(
            row, 0,
            new QTableWidgetItem(mod_display_name(patch.mod_id)));
        patches_table_->setItem(
            row, 1,
            new QTableWidgetItem(
                QString::fromStdString(patch.archive_path)));
        auto* status = new QTableWidgetItem(
            patch_allowed_.value(key) ? tr("Allow") : tr("Deny"));
        status->setData(Qt::UserRole, key);
        patches_table_->setItem(row, 2, status);
        patches_table_->setItem(
            row, 3,
            new QTableWidgetItem(
                QString::fromStdString(patch.target_path)));
        ++row;
    }
    connect(patches_table_, &QTableWidget::cellChanged, this,
            &ModpackInstallWizard::on_patch_cell_changed);
    connect(patches_table_, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column != 2 || patches_table_ == nullptr) return;
                auto* item = patches_table_->item(row, column);
                if (item == nullptr) return;
                const QString key = item->data(Qt::UserRole).toString();
                const bool allowed = !patch_allowed_.value(key, false);
                patch_allowed_[key] = allowed;
                const bool blocked = patches_table_->blockSignals(true);
                item->setText(allowed ? tr("Allow") : tr("Deny"));
                patches_table_->blockSignals(blocked);
            });
    layout->addWidget(patches_table_, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* allow_all = new QPushButton(tr("Allow All"), page);
    auto* deny_all = new QPushButton(tr("Deny All"), page);
    connect(allow_all, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_patch_allow_all);
    connect(deny_all, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_patch_deny_all);
    buttons->addWidget(allow_all);
    buttons->addWidget(deny_all);
    layout->addLayout(buttons);
    return page;
}

QWidget* ModpackInstallWizard::build_finishing_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Applying the modpack..."), page));

    finishing_table_ = new QTableWidget(page);
    finishing_table_->setColumnCount(2);
    finishing_table_->setHorizontalHeaderLabels({tr(""), tr("Action")});
    finishing_table_->horizontalHeader()->setStretchLastSection(true);
    finishing_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    finishing_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QStringList actions = {
        tr("Creating separators from tree structure"),
        tr("Parenting mods (nested separators)"),
        tr("Applying load order from tree"),
        tr("Platform setup (proton, Steam overlay, prefix files)"),
    };
    finishing_table_->setRowCount(actions.size());
    for (int i = 0; i < actions.size(); ++i) {
        finishing_table_->setItem(i, 0, new QTableWidgetItem(QStringLiteral("\u25CB")));
        finishing_table_->setItem(i, 1, new QTableWidgetItem(actions[i]));
        finishing_done_[i] = false;
    }
    layout->addWidget(finishing_table_, 1);
    return page;
}

QWidget* ModpackInstallWizard::build_end_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    auto* ready = new QLabel(tr("Your modpack is ready!"), page);
    QFont title_font = ready->font();
    title_font.setPointSize(title_font.pointSize() + 6);
    title_font.setBold(true);
    ready->setFont(title_font);
    ready->setAlignment(Qt::AlignCenter);
    layout->addWidget(ready);

    QString closing;
    if (pack_.instructions && !pack_.instructions->empty()) {
        closing = QString::fromStdString(*pack_.instructions);
    }
    if (closing.isEmpty()) {
        closing = tr("Thanks for installing %1.")
                      .arg(QString::fromStdString(
                          pack_.manifest.info.name.empty()
                              ? pack_.manifest.id
                              : pack_.manifest.info.name));
    }
    auto* notes = new QLabel(closing, page);
    notes->setWordWrap(true);
    notes->setAlignment(Qt::AlignCenter);
    layout->addWidget(notes);
    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Page hooks + slots
// ---------------------------------------------------------------------------

void ModpackInstallWizard::on_page_entered(int index) {
    if (steps_[index].id == Step::Downloads) {
        refresh_downloads_ui();
    } else if (steps_[index].id == Step::Finishing) {
        start_finishing_animation();
    }
}

void ModpackInstallWizard::refresh_downloads_ui() {
    if (downloads_table_ == nullptr) return;
    // Phase-ordered: phase 0 first, then 1, ...
    std::vector<const ModEntry*> ordered;
    for (const auto& mod : pack_.mods) ordered.push_back(&mod);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ModEntry* a, const ModEntry* b) {
                         return a->phase < b->phase;
                     });

    downloads_table_->setRowCount(static_cast<int>(ordered.size()));
    int done = 0;
    int row = 0;
    for (const ModEntry* mod : ordered) {
        const QString mid = QString::fromStdString(mod->id);
        const QString status =
            download_status_.value(mid, QStringLiteral("pending"));
        if (status == QStringLiteral("downloaded")) ++done;

        const QString label = QStringLiteral("%1 %2").arg(
            status_icon(status),
            QString::fromStdString(
                mod->name.empty() ? mod->id : mod->name));
        downloads_table_->setItem(row, 0, new QTableWidgetItem(
            QStringLiteral("phase %1").arg(mod->phase)));
        auto* name_item = new QTableWidgetItem(label);
        name_item->setData(Qt::UserRole, mid);
        name_item->setToolTip(category_name(mod->category));
        downloads_table_->setItem(row, 1, name_item);
        downloads_table_->setItem(
            row, 2,
            new QTableWidgetItem(source_provider(*mod) + QStringLiteral(" - ") +
                                 source_detail(*mod)));
        downloads_table_->setItem(
            row, 3, new QTableWidgetItem(format_size(source_file_size(*mod))));

        QWidget* cell = nullptr;
        if (status == QStringLiteral("pending") ||
            status == QStringLiteral("failed")) {
            auto* dl = new QPushButton(
                status == QStringLiteral("failed") ? tr("Retry") : tr("Get"),
                downloads_table_);
            dl->setProperty("mod_id", mid);
            connect(dl, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_one);
            cell = dl;
        } else if (status == QStringLiteral("downloading")) {
            cell = new QLabel(status_icon(status) + tr(" active..."),
                              downloads_table_);
        } else if (mod->category == ModCategory::Optional &&
                   status != QStringLiteral("downloaded")) {
            auto* skip = new QPushButton(tr("Skip"), downloads_table_);
            skip->setProperty("mod_id", mid);
            connect(skip, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_skip_optional);
            cell = skip;
        } else {
            cell = new QLabel(status_icon(status), downloads_table_);
        }
        // Optional pending mods get a skip affordance next to Get: handled
        // by turning the single cell into a row widget with both buttons.
        if ((status == QStringLiteral("pending") ||
             status == QStringLiteral("failed")) &&
            mod->category == ModCategory::Optional) {
            auto* box = new QWidget(downloads_table_);
            auto* box_layout = new QHBoxLayout(box);
            box_layout->setContentsMargins(0, 0, 0, 0);
            box_layout->addWidget(cell);
            auto* skip = new QPushButton(tr("Skip"), box);
            skip->setProperty("mod_id", mid);
            connect(skip, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_skip_optional);
            box_layout->addWidget(skip);
            cell = box;
        }
        downloads_table_->setCellWidget(row, 4, cell);
        ++row;
    }

    const int total = static_cast<int>(ordered.size());
    downloads_bar_->setMaximum(total == 0 ? 1 : total);
    downloads_bar_->setValue(done);
    downloads_bar_->setFormat(tr("%1 of %2 downloaded").arg(done).arg(total));
    refresh_next_enabled();
}

void ModpackInstallWizard::on_browse_path(QLineEdit* edit) {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose directory"), edit->text());
    if (!dir.isEmpty()) edit->setText(dir);
}

void ModpackInstallWizard::on_choice_toggled() {
    QObject* sender_obj = sender();
    if (sender_obj == nullptr) return;
    const QString gid = sender_obj->property("group_id").toString();
    const QString mid = sender_obj->property("mod_id").toString();
    const auto* radio = qobject_cast<const QRadioButton*>(sender_obj);
    if (radio != nullptr) {
        // exactly-one: radio exclusivity already enforces one selection.
        if (radio->isChecked()) choice_selections_[gid] = QStringList(mid);
        return;
    }
    // at-most-one: keep zero or one checked.
    const auto* check = qobject_cast<const QCheckBox*>(sender_obj);
    if (check == nullptr) return;
    QStringList selected = choice_selections_.value(gid);
    selected.removeAll(mid);
    if (check->isChecked()) {
        selected.clear();  // at most one: new pick replaces the old one.
        selected << mid;
        // Uncheck the sibling boxes without recursing through signals.
        auto* page = stack_->widget(static_cast<int>(Step::Choices));
        const auto boxes = page->findChildren<QCheckBox*>();
        for (auto* sibling : boxes) {
            if (sibling != check &&
                sibling->property("group_id").toString() == gid) {
                const bool blocked = sibling->blockSignals(true);
                sibling->setChecked(false);
                sibling->blockSignals(blocked);
            }
        }
    }
    choice_selections_[gid] = selected;
}

void ModpackInstallWizard::on_ini_toggled() {
    const auto* check = qobject_cast<const QCheckBox*>(sender());
    if (check == nullptr) return;
    ini_enabled_[check->property("tweak_id").toString()] = check->isChecked();
}

void ModpackInstallWizard::on_download_one() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    const QString mid = button->property("mod_id").toString();
    download_status_[mid] = QStringLiteral("downloading");
    refresh_downloads_ui();
    // Simulated fetch; the real download pipeline wires in here.
    sim_mod_id_ = mid;
    sim_ticks_ = 0;
    disconnect(sim_timer_, nullptr, nullptr, nullptr);
    connect(sim_timer_, &QTimer::timeout, this,
            &ModpackInstallWizard::on_download_sim_tick);
    sim_timer_->start(120);
}

void ModpackInstallWizard::on_download_sim_tick() {
    if (++sim_ticks_ < 4) return;  // brief "active..." state
    sim_timer_->stop();
    download_status_[sim_mod_id_] = QStringLiteral("downloaded");
    refresh_downloads_ui();
}

void ModpackInstallWizard::on_download_skip_optional() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    download_status_[button->property("mod_id").toString()] =
        QStringLiteral("skipped");
    refresh_downloads_ui();
}

void ModpackInstallWizard::on_run_tool_one() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    tool_running_id_ = button->property("exe_id").toString();
    tool_status_[tool_running_id_] = QStringLiteral("running");
    tool_sim_ticks_ = 0;
    if (tools_status_ != nullptr) {
        tools_status_->setText(tr("Running %1...").arg(tool_running_id_));
    }
    disconnect(sim_timer_, nullptr, nullptr, nullptr);
    connect(sim_timer_, &QTimer::timeout, this,
            &ModpackInstallWizard::on_tool_sim_tick);
    sim_timer_->start(150);
}

void ModpackInstallWizard::on_tool_sim_tick() {
    if (++tool_sim_ticks_ < 3) return;
    sim_timer_->stop();
    tool_status_[tool_running_id_] = QStringLiteral("done");
    if (tools_status_ != nullptr) {
        tools_status_->setText(tr("%1 finished.").arg(tool_running_id_));
    }
    // Refresh the status column in place.
    if (tools_table_ != nullptr) {
        for (int row = 0; row < tools_table_->rowCount(); ++row) {
            auto* id_item = tools_table_->item(row, 0);
            if (id_item != nullptr &&
                id_item->data(Qt::UserRole).toString() == tool_running_id_) {
                tools_table_->setItem(
                    row, 2, new QTableWidgetItem(QStringLiteral("done")));
                auto* skip = new QPushButton(tr("Skip"), tools_table_);
                skip->setProperty("exe_id", tool_running_id_);
                connect(skip, &QPushButton::clicked, this,
                        &ModpackInstallWizard::on_run_tool_skip);
                tools_table_->setCellWidget(row, 3, skip);
            }
        }
    }
}

void ModpackInstallWizard::on_run_tool_skip() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    const QString eid = button->property("exe_id").toString();
    tool_status_[eid] = QStringLiteral("skipped");
    if (tools_table_ != nullptr) {
        for (int row = 0; row < tools_table_->rowCount(); ++row) {
            auto* id_item = tools_table_->item(row, 0);
            if (id_item != nullptr &&
                id_item->data(Qt::UserRole).toString() == eid) {
                tools_table_->setItem(
                    row, 2, new QTableWidgetItem(QStringLiteral("skipped")));
            }
        }
    }
}

void ModpackInstallWizard::on_patch_allow_all() {
    for (auto it = patch_allowed_.begin(); it != patch_allowed_.end(); ++it) {
        it.value() = true;
    }
    if (patches_table_ != nullptr) {
        const bool blocked = patches_table_->blockSignals(true);
        for (int row = 0; row < patches_table_->rowCount(); ++row) {
            patches_table_->item(row, 2)->setText(tr("Allow"));
        }
        patches_table_->blockSignals(blocked);
    }
}

void ModpackInstallWizard::on_patch_deny_all() {
    for (auto it = patch_allowed_.begin(); it != patch_allowed_.end(); ++it) {
        it.value() = false;
    }
    if (patches_table_ != nullptr) {
        const bool blocked = patches_table_->blockSignals(true);
        for (int row = 0; row < patches_table_->rowCount(); ++row) {
            patches_table_->item(row, 2)->setText(tr("Deny"));
        }
        patches_table_->blockSignals(blocked);
    }
}

void ModpackInstallWizard::on_patch_cell_changed(int row, int column) {
    if (patches_table_ == nullptr || column != 2) return;
    auto* item = patches_table_->item(row, column);
    if (item == nullptr) return;
    // Status cell cycles Allow/Deny on double-click edit; single click on
    // the row toggles consent directly.
    const QString key = item->data(Qt::UserRole).toString();
    const bool allowed = item->text().compare(tr("Allow"), Qt::CaseInsensitive) == 0;
    patch_allowed_[key] = allowed;
}

void ModpackInstallWizard::start_finishing_animation() {
    if (finishing_table_ == nullptr || finishing_started_) return;
    finishing_started_ = true;
    finishing_tick_ = 0;
    disconnect(sim_timer_, nullptr, nullptr, nullptr);
    connect(sim_timer_, &QTimer::timeout, this,
            &ModpackInstallWizard::on_finishing_tick);
    sim_timer_->start(250);
}

void ModpackInstallWizard::on_finishing_tick() {
    if (finishing_table_ == nullptr) {
        sim_timer_->stop();
        return;
    }
    if (finishing_tick_ >= finishing_table_->rowCount()) {
        sim_timer_->stop();
        return;
    }
    finishing_table_->setItem(
        finishing_tick_, 0, new QTableWidgetItem(QStringLiteral("\u2713")));
    finishing_done_[finishing_tick_] = true;
    ++finishing_tick_;
    if (finishing_tick_ >= finishing_table_->rowCount()) sim_timer_->stop();
}

}  // namespace ui
