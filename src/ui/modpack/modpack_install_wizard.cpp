#include "ui/modpack/modpack_install_wizard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace ui {

ModpackInstallWizard::ModpackInstallWizard(engine::gmmpack::Gmmpack pack,
                                           Mode mode, QWidget* parent)
    : QDialog(parent), pack_(std::move(pack)), mode_(mode) {
    const QString pack_name =
        QString::fromStdString(pack_.manifest.info.name);
    setWindowTitle(pack_name.isEmpty()
                       ? tr("Install Modpack")
                       : tr("Install Modpack: %1").arg(pack_name));
    resize(900, 600);

    build_steps();

    auto* layout = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter);

    sidebar_ = new QListWidget(splitter);
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
    for (const auto& step : steps_) {
        // Placeholder until Workspace-on1c implements the real step content.
        auto* page = new QLabel(
            tr("%1\n\nTODO").arg(step.title), stack_);
        page->setAlignment(Qt::AlignCenter);
        stack_->addWidget(page);
    }
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

void ModpackInstallWizard::go_to(int index) {
    if (index < 0 || index >= static_cast<int>(steps_.size())) return;
    if (steps_[index].skipped) return;
    current_ = index;
    steps_[current_].visited = true;
    stack_->setCurrentIndex(current_);
    refresh_chrome();
}

void ModpackInstallWizard::on_next() {
    if (current_ >= static_cast<int>(steps_.size()) - 1) {
        accept();
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
}

}  // namespace ui
