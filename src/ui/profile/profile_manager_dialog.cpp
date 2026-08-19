#include "ui/profile/profile_manager_dialog.h"

#include "engine/core/log/logger.h"
#include "engine/profile/profile.h"
#include "engine/profile/profile_creation.h"
#include "ui/profile/profile_create_dialog.h"
#include "ui/profile/profile_settings_widget.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

namespace {

// Human-readable result for a failed profile operation (the engine returns
// error strings; the UI shows them in a message box).
void show_error(QWidget* parent, const QString& title, const std::string& error) {
    QMessageBox::warning(parent, title, QString::fromStdString(error));
}

}  // namespace

ProfileManagerDialog::ProfileManagerDialog(const std::filesystem::path& profiles_dir,
                                           const QString& active_profile,
                                           const QString& default_profile, QWidget* parent)
    : QDialog(parent),
      profiles_dir_(profiles_dir),
      active_profile_(active_profile),
      default_profile_(default_profile) {
    setWindowTitle(tr("Profile Manager"));
    setMinimumSize(480, 420);

    auto* root = new QVBoxLayout(this);

    list_ = new QListWidget(this);
    root->addWidget(list_, 1);

    // Per-profile settings (MO2's ProfilesDialog checkboxes).
    auto* settings_group = new QGroupBox(tr("Profile settings"), this);
    auto* settings_layout = new QVBoxLayout(settings_group);
    settings_widget_ = new ProfileSettingsWidget(settings_group);
    settings_layout->addWidget(settings_widget_);
    root->addWidget(settings_group);

    // Action buttons.
    auto* buttons = new QHBoxLayout;
    auto* create_btn = new QPushButton(tr("Create"), this);
    copy_btn_ = new QPushButton(tr("Copy"), this);
    rename_btn_ = new QPushButton(tr("Rename"), this);
    delete_btn_ = new QPushButton(tr("Delete"), this);
    default_btn_ = new QPushButton(tr("Set Default"), this);
    buttons->addWidget(create_btn);
    buttons->addWidget(copy_btn_);
    buttons->addWidget(rename_btn_);
    buttons->addWidget(delete_btn_);
    buttons->addWidget(default_btn_);
    buttons->addStretch(1);
    root->addLayout(buttons);

    auto* bottom = new QHBoxLayout;
    select_btn_ = new QPushButton(tr("Select"), this);
    select_btn_->setDefault(true);
    auto* close_btn = new QPushButton(tr("Close"), this);
    bottom->addStretch(1);
    bottom->addWidget(select_btn_);
    bottom->addWidget(close_btn);
    root->addLayout(bottom);

    connect(create_btn, &QPushButton::clicked, this, &ProfileManagerDialog::on_create);
    connect(copy_btn_, &QPushButton::clicked, this, &ProfileManagerDialog::on_copy);
    connect(rename_btn_, &QPushButton::clicked, this, &ProfileManagerDialog::on_rename);
    connect(delete_btn_, &QPushButton::clicked, this, &ProfileManagerDialog::on_delete_profile);
    connect(default_btn_, &QPushButton::clicked, this, &ProfileManagerDialog::on_set_default);
    connect(select_btn_, &QPushButton::clicked, this, &ProfileManagerDialog::on_select);
    connect(close_btn, &QPushButton::clicked, this, &QDialog::reject);
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                Q_UNUSED(current)
                on_selection_changed();
            });
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { on_select(); });

    // Per-profile settings toggles persist immediately (MO2 behavior).
    connect(settings_widget_, &ProfileSettingsWidget::settings_changed, this,
            [this]() {
                const QString name = current_item_name();
                if (name.isEmpty()) {
                    return;
                }
                engine::profile::Profile profile(profiles_dir_ / name.toStdString());
                profile.set_local_saves(settings_widget_->local_saves());
                profile.set_local_settings(settings_widget_->local_settings());
                profile.set_automatic_archive_invalidation(
                    settings_widget_->archive_invalidation());
                profile.save_settings();
            });

    refresh_list();
}

QString ProfileManagerDialog::current_item_name() const {
    auto* item = list_->currentItem();
    if (!item) {
        return {};
    }
    return item->data(Qt::UserRole).toString();
}

void ProfileManagerDialog::refresh_list() {
    const QString previous = current_item_name();
    list_->clear();

    const auto names = engine::profile::list_profiles(profiles_dir_);
    for (const auto& name : names) {
        const QString qname = QString::fromStdString(name);
        QString label = qname;
        if (qname == active_profile_) {
            label += tr("  (active)");
        } else if (qname == default_profile_) {
            label += tr("  (default)");
        }
        auto* item = new QListWidgetItem(label, list_);
        item->setData(Qt::UserRole, qname);
        if (qname == active_profile_) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }

    // Restore the previous selection (or the active profile on first open).
    if (!previous.isEmpty()) {
        for (int i = 0; i < list_->count(); ++i) {
            if (list_->item(i)->data(Qt::UserRole).toString() == previous) {
                list_->setCurrentRow(i);
                break;
            }
        }
    } else if (list_->count() > 0) {
        list_->setCurrentRow(0);
    }
    on_selection_changed();
}

void ProfileManagerDialog::on_create() {
    QStringList existing;
    for (int i = 0; i < list_->count(); ++i) {
        existing << list_->item(i)->data(Qt::UserRole).toString();
    }

    ProfileCreateDialog dialog(existing, {}, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const std::string name = dialog.profile_name().toStdString();
    auto result = engine::profile::create_fresh_profile(profiles_dir_, name);
    if (!result.success) {
        show_error(this, tr("Create Profile"), result.error);
        return;
    }
    refresh_list();
    emit profiles_changed();
}

void ProfileManagerDialog::on_copy() {
    const QString source = current_item_name();
    if (source.isEmpty()) {
        return;
    }
    QStringList existing;
    for (int i = 0; i < list_->count(); ++i) {
        existing << list_->item(i)->data(Qt::UserRole).toString();
    }

    ProfileCreateDialog dialog(existing, source, this);
    dialog.setWindowTitle(tr("Copy Profile"));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const std::string new_name = dialog.profile_name().toStdString();
    auto result = engine::profile::copy_profile(profiles_dir_, new_name,
                                                profiles_dir_ / source.toStdString());
    if (!result.success) {
        show_error(this, tr("Copy Profile"), result.error);
        return;
    }
    refresh_list();
    emit profiles_changed();
}

void ProfileManagerDialog::on_rename() {
    const QString old_name = current_item_name();
    if (old_name.isEmpty()) {
        return;
    }
    if (old_name == active_profile_) {
        QMessageBox::warning(this, tr("Renaming active profile"),
                             tr("The active profile cannot be renamed. Please switch to a "
                                "different profile first."));
        return;
    }

    bool ok = false;
    const QString new_name =
        QInputDialog::getText(this, tr("Rename Profile"), tr("New name:"),
                              QLineEdit::Normal, old_name, &ok);
    if (!ok || new_name.trimmed().isEmpty() || new_name.trimmed() == old_name) {
        return;
    }

    std::string error;
    if (!engine::profile::rename_profile(profiles_dir_, old_name.toStdString(),
                                         new_name.trimmed().toStdString(), &error)) {
        show_error(this, tr("Rename Profile"), error);
        return;
    }
    refresh_list();
    emit profiles_changed();
}

void ProfileManagerDialog::on_delete_profile() {
    const QString name = current_item_name();
    if (name.isEmpty()) {
        return;
    }
    if (name == active_profile_) {
        QMessageBox::warning(this, tr("Deleting active profile"),
                             tr("The active profile cannot be deleted. Please switch to a "
                                "different profile first."));
        return;
    }

    const auto answer = QMessageBox::question(
        this, tr("Delete Profile"),
        tr("Are you sure you want to delete profile \"%1\"? This removes the profile "
           "directory and all profile-specific files (including profile-specific save "
           "games, if any).")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    engine::profile::Profile profile(profiles_dir_ / name.toStdString());
    const auto result = profile.remove(/*is_active=*/false);
    switch (result) {
    case engine::profile::ProfileRemoveResult::Removed:
        break;
    case engine::profile::ProfileRemoveResult::NotFound:
        show_error(this, tr("Delete Profile"), "profile directory does not exist");
        return;
    case engine::profile::ProfileRemoveResult::ActiveProfile:
        show_error(this, tr("Delete Profile"), "the active profile cannot be deleted");
        return;
    case engine::profile::ProfileRemoveResult::PermissionDenied:
        show_error(this, tr("Delete Profile"), "could not delete the profile directory");
        return;
    case engine::profile::ProfileRemoveResult::PartialFailure:
        show_error(this, tr("Delete Profile"),
                   "some profile contents were deleted, but the directory still exists");
        return;
    }

    if (name == default_profile_) {
        default_profile_.clear();
        emit default_profile_changed({});
    }
    refresh_list();
    emit profiles_changed();
}

void ProfileManagerDialog::on_set_default() {
    const QString name = current_item_name();
    if (name.isEmpty()) {
        return;
    }
    default_profile_ = name;
    emit default_profile_changed(name);
    refresh_list();
}

void ProfileManagerDialog::on_select() {
    const QString name = current_item_name();
    if (name.isEmpty()) {
        return;
    }
    selected_ = name;
    accept();
}

void ProfileManagerDialog::on_selection_changed() {
    const QString name = current_item_name();
    const bool has_selection = !name.isEmpty();
    copy_btn_->setEnabled(has_selection);
    rename_btn_->setEnabled(has_selection);
    delete_btn_->setEnabled(has_selection);
    default_btn_->setEnabled(has_selection);
    select_btn_->setEnabled(has_selection);
    load_settings_for(name);
}

void ProfileManagerDialog::load_settings_for(const QString& name) {
    // The widget blocks its own signals while loading, so this never triggers
    // the save-on-toggle handler.
    settings_widget_->set_profile(name.isEmpty()
                                      ? std::filesystem::path{}
                                      : profiles_dir_ / name.toStdString());
}

}  // namespace ui