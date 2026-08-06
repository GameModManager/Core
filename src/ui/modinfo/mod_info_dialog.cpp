#include "ui/modinfo/mod_info_dialog.h"

#include "ui/modinfo/mod_info_tab.h"
#include "ui/settings/settings.h"

// Tabs (order matters — must match ModInfoTabId).
#include "ui/modinfo/categories_tab.h"
#include "ui/modinfo/config_files_tab.h"
#include "ui/modinfo/conflicts_tab.h"
#include "ui/modinfo/esps_tab.h"
#include "ui/modinfo/filetree_tab.h"
#include "ui/modinfo/images_tab.h"
#include "ui/modinfo/notes_tab.h"
#include "ui/modinfo/source_tab.h"
#include "ui/modinfo/text_files_tab.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMoveEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ui {

ModInfoDialog::ModInfoDialog(std::vector<ModInfoData> mods, int index,
                             ModInfoTabId initial_tab, QWidget* parent)
    : QDialog(parent)
    , mods_(std::move(mods))
    , index_(index) {
    setWindowTitle(tr("Mod Information"));
    resize(735, 534);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(false);
    tabs_->setMovable(true);
    layout->addWidget(tabs_, 1);

    const auto make_tab = [&](ModInfoTab* tab, const QString& title) {
        tab->set_tab_id(static_cast<ModInfoTabId>(tab_order_.size()));
        tab_order_.push_back(tab);
        tabs_->addTab(tab, title);
    };

    make_tab(new TextFilesTab(this), tr("Text Files"));
    make_tab(new ConfigFilesTab(this), tr("Config Files"));
    make_tab(new ImagesTab(this), tr("Images"));
    make_tab(new EspsTab(this), tr("ESP Files"));
    make_tab(new ConflictsInfoTab(this), tr("Conflicts"));
    make_tab(new CategoriesTab(this), tr("Categories"));
    make_tab(new SourceTab(this), tr("Source"));
    make_tab(new NotesTab(this), tr("Notes"));
    make_tab(new FiletreeTab(this), tr("Filetree"));

    tab_activated_.assign(tab_order_.size(), false);

    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (i < 0 || i >= static_cast<int>(tab_order_.size())) return;
        if (!tab_activated_[static_cast<size_t>(i)]) {
            tab_activated_[static_cast<size_t>(i)] = true;
            tab_order_[static_cast<size_t>(i)]->first_activation();
        }
    });

    // --- bottom bar: prev / mod name / next | delete | close ---
    auto* bar = new QWidget(this);
    auto* bar_layout = new QHBoxLayout(bar);
    bar_layout->setContentsMargins(0, 4, 0, 0);

    prev_btn_ = new QPushButton(QChar(0x226A), bar);  // "≪"
    prev_btn_->setToolTip(tr("Previous mod"));
    next_btn_ = new QPushButton(QChar(0x226B), bar);  // "≫"
    next_btn_->setToolTip(tr("Next mod"));
    bar_layout->addWidget(prev_btn_);
    bar_layout->addWidget(next_btn_);

    mod_name_ = new QLabel(bar);
    QFont name_font = mod_name_->font();
    name_font.setBold(true);
    mod_name_->setFont(name_font);
    mod_name_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar_layout->addWidget(mod_name_, 1);

    delete_btn_ = new QPushButton(tr("Delete"), bar);
    delete_btn_->setObjectName("deleteModBtn");
    bar_layout->addWidget(delete_btn_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, bar);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bar_layout->addWidget(buttons);

    layout->addWidget(bar);

    connect(prev_btn_, &QPushButton::clicked, this, [this]() {
        const int target = next_mod_index(index_, -1);
        if (target >= 0) switch_to(target);
    });
    connect(next_btn_, &QPushButton::clicked, this, [this]() {
        const int target = next_mod_index(index_, +1);
        if (target >= 0) switch_to(target);
    });
    connect(delete_btn_, &QPushButton::clicked, this,
            &ModInfoDialog::on_delete_mod);

    restore_geometry();

    // Load the initial mod's data BEFORE activating any tab: setCurrentIndex
    // below fires first_activation(), which reads tab data (and switch_to()
    // early-returns when index == index_, so it can't be relied on here).
    load_index(index_);

    // Place on the requested tab, or the last-used one.
    int tab_index = static_cast<int>(initial_tab);
    if (tab_index < 0 || tab_index >= tabs_->count())
        tab_index = Settings::instance().modinfo_last_tab();
    tab_index = qBound(0, tab_index, tabs_->count() - 1);
    tabs_->setCurrentIndex(tab_index);
}

ModInfoDialog::~ModInfoDialog() = default;

void ModInfoDialog::load_index(int index) {
    index_ = index;
    const auto& data = mods_[static_cast<size_t>(index_)];
    for (auto* tab : tab_order_) {
        tab->set_current(data);
        tab->set_mod(data);
        tab->restore_state();
    }

    mod_name_->setText(data.name);
    prev_btn_->setEnabled(next_mod_index(index_, -1) >= 0);
    next_btn_->setEnabled(next_mod_index(index_, +1) >= 0);

    const bool deletable = !data.is_separator && !data.is_overwrite &&
                           !data.is_merged && !data.is_game_native &&
                           static_cast<bool>(data.delete_mod);
    delete_btn_->setEnabled(deletable);
}

void ModInfoDialog::switch_to(int index) {
    if (index == index_) return;
    if (!can_switch()) return;

    for (auto* tab : tab_order_) tab->save_state();

    load_index(index);
}

int ModInfoDialog::next_mod_index(int from, int dir) const {
    for (int i = from + dir; i >= 0 && i < static_cast<int>(mods_.size()); i += dir) {
        if (!mods_[static_cast<size_t>(i)].is_separator) return i;
    }
    return -1;
}

void ModInfoDialog::reload_current(ModInfoData data) {
    if (index_ < 0 || index_ >= static_cast<int>(mods_.size())) return;
    if (!can_switch()) return;
    for (auto* tab : tab_order_) tab->save_state();
    mods_[static_cast<size_t>(index_)] = std::move(data);
    const auto& cur = mods_[static_cast<size_t>(index_)];
    for (auto* tab : tab_order_) {
        tab->set_current(cur);
        tab->set_mod(cur);
        tab->restore_state();
    }
    mod_name_->setText(cur.name);
    tabs_->setCurrentIndex(static_cast<int>(ModInfoTabId::Conflicts));
}

QString ModInfoDialog::current_mod_id() const {
    if (index_ < 0 || index_ >= static_cast<int>(mods_.size())) return {};
    return mods_[static_cast<size_t>(index_)].id;
}

bool ModInfoDialog::can_switch() const {
    for (auto* tab : tab_order_) {
        if (!tab->can_close()) return false;
    }
    return true;
}

void ModInfoDialog::on_delete_mod() {
    if (index_ < 0 || index_ >= static_cast<int>(mods_.size())) return;
    auto& data = mods_[static_cast<size_t>(index_)];
    if (data.is_separator || data.is_overwrite || data.is_merged ||
        data.is_game_native || !data.delete_mod) {
        return;
    }

    if (QMessageBox::question(this, tr("Delete Mod"),
                              tr("Delete the mod \"%1\"?").arg(data.name),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    if (data.delete_mod()) {
        persist_geometry();
        accept();  // the mod list refresh invalidates the data we hold
    }
}

void ModInfoDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete) {
        on_delete_mod();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ModInfoDialog::persist_geometry() {
    Settings::instance().set_modinfo_last_tab(tabs_->currentIndex());
    Settings::instance().set_modinfo_window_geometry(saveGeometry());
}

void ModInfoDialog::restore_geometry() {
    const auto geo = Settings::instance().modinfo_window_geometry();
    if (!geo.isEmpty()) restoreGeometry(geo);
}

void ModInfoDialog::closeEvent(QCloseEvent* event) {
    if (!can_switch()) {
        event->ignore();
        return;
    }
    for (auto* tab : tab_order_) tab->save_state();
    persist_geometry();
    QDialog::closeEvent(event);
}

void ModInfoDialog::moveEvent(QMoveEvent* event) {
    QDialog::moveEvent(event);
    // Persist on move so a crashed/dropped dialog still restores its spot.
    Settings::instance().set_modinfo_window_geometry(saveGeometry());
}

}  // namespace ui
