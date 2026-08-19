#include "ui/widgets/instance_switcher_dialog.h"
#include "ui/widgets/instance_switcher_content_widget.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace ui {

InstanceSwitcherDialog::InstanceSwitcherDialog(engine::PluginLoader* plugins,
                                               QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Switch Instance"));
    resize(520, 400);

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // The mode-agnostic content: instance list + create button. The dialog
    // only adds the OK/Cancel row that carries the accept/reject semantics.
    content_ = new InstanceSwitcherContentWidget(plugins, this);
    main_layout->addWidget(content_, 1);

    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->setContentsMargins(16, 0, 16, 12);
    bottom_layout->addStretch();

    auto* ok_btn = new QPushButton(tr("OK"), this);
    auto* cancel_btn = new QPushButton(tr("Cancel"), this);
    ok_btn->setDefault(true);
    bottom_layout->addWidget(ok_btn);
    bottom_layout->addWidget(cancel_btn);

    main_layout->addLayout(bottom_layout);

    // Connections
    connect(ok_btn, &QPushButton::clicked, this, &InstanceSwitcherDialog::on_ok);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
    // Double-click on an instance row accepts the dialog with that selection.
    connect(content_, &InstanceSwitcherContentWidget::instance_selected, this,
            [this](const QString&) { on_ok(); });
    // Create button: remember the request and accept so the caller runs the
    // GameSelectionWidget create flow.
    connect(content_, &InstanceSwitcherContentWidget::create_new_instance, this,
            [this]() {
                create_requested_ = true;
                emit create_new_instance();
                accept();
            });
}

void InstanceSwitcherDialog::load_instances(const std::string& instances_dir) {
    content_->load_instances(instances_dir);
}

QString InstanceSwitcherDialog::selected_instance() const {
    return content_->selected_instance();
}

void InstanceSwitcherDialog::on_ok() {
    if (!content_->selected_instance().isEmpty())
        accept();
}

}  // namespace ui
