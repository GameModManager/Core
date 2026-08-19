#include "ui/settings/settings_dialog.h"
#include "ui/settings/settings_content_widget.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(engine::StyleManager *style_manager,
                               const QString &native_style_name,
                               const std::filesystem::path &instance_root,
                               engine::PluginLoader *plugin_loader,
                               QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(tr("Settings"));
  // Min geometry H634 x W723: the Plugins tab's two columns need the width
  // (left plugin list ~half, right info pane ~half) at minimum window size.
  setMinimumSize(ui::kSettingsMinWidth, ui::kSettingsMinHeight);

  auto *layout = new QVBoxLayout(this);
  content_ = new ui::SettingsContentWidget(style_manager, native_style_name,
                                           instance_root, plugin_loader, this);
  layout->addWidget(content_, 1); // tab pages fill the whole window height

  auto *btn_box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(btn_box, &QDialogButtonBox::rejected, this, &QDialog::close);
  layout->addWidget(btn_box);

  // Forward the mode toggle so the embedding host reacts to the checkbox
  // while the dialog is open.
  connect(content_, &ui::SettingsContentWidget::full_ui_mode_toggled, this,
          &SettingsDialog::full_ui_mode_toggled);
}

void SettingsDialog::closeEvent(QCloseEvent *event) {
  // Let an embedding host (Full UI tab mode) drop the tab that holds this
  // dialog when the user closes it (Close button, window X, ...).
  emit closed();
  QDialog::closeEvent(event);
}
