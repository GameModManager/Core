#include "ui/profile/profile_settings_widget.h"

#include "engine/profile/profile.h"

#include <QCheckBox>
#include <QVBoxLayout>

namespace ui {

ProfileSettingsWidget::ProfileSettingsWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    local_saves_box_ = new QCheckBox(tr("Local Saves"), this);
    local_settings_box_ = new QCheckBox(tr("Local Settings"), this);
    invalidation_box_ = new QCheckBox(tr("Archive Invalidation"), this);
    layout->addWidget(local_saves_box_);
    layout->addWidget(local_settings_box_);
    layout->addWidget(invalidation_box_);

    connect(local_saves_box_, &QCheckBox::toggled, this,
            [this](bool) { emit settings_changed(); });
    connect(local_settings_box_, &QCheckBox::toggled, this,
            [this](bool) { emit settings_changed(); });
    connect(invalidation_box_, &QCheckBox::toggled, this,
            [this](bool) { emit settings_changed(); });
}

void ProfileSettingsWidget::set_profile(const std::filesystem::path& profile_dir) {
    // Block signals so loading never triggers settings_changed().
    local_saves_box_->blockSignals(true);
    local_settings_box_->blockSignals(true);
    invalidation_box_->blockSignals(true);

    if (profile_dir.empty()) {
        local_saves_box_->setChecked(false);
        local_settings_box_->setChecked(false);
        invalidation_box_->setChecked(false);
        local_saves_box_->setEnabled(false);
        local_settings_box_->setEnabled(false);
        invalidation_box_->setEnabled(false);
    } else {
        engine::profile::Profile profile(profile_dir);
        local_saves_box_->setChecked(profile.local_saves());
        local_settings_box_->setChecked(profile.local_settings());
        invalidation_box_->setChecked(profile.automatic_archive_invalidation());
        local_saves_box_->setEnabled(true);
        local_settings_box_->setEnabled(true);
        invalidation_box_->setEnabled(true);
    }

    local_saves_box_->blockSignals(false);
    local_settings_box_->blockSignals(false);
    invalidation_box_->blockSignals(false);
}

bool ProfileSettingsWidget::local_saves() const {
    return local_saves_box_->isChecked();
}

bool ProfileSettingsWidget::local_settings() const {
    return local_settings_box_->isChecked();
}

bool ProfileSettingsWidget::archive_invalidation() const {
    return invalidation_box_->isChecked();
}

}  // namespace ui