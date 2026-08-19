#include "ui/widgets/instance_statistics_dialog.h"

#include <QVBoxLayout>

#include "ui/widgets/stats_content_widget.h"

namespace ui {

InstanceStatisticsDialog::InstanceStatisticsDialog(
    const std::filesystem::path& instance_root,
    const std::filesystem::path& cache_dir,
    int total_mods,
    QWidget* parent)
    : QDialog(parent) {

    setWindowTitle(tr("Instance Statistics"));
    setMinimumWidth(420);

    // The content widget owns the stats layout (including its margins), so
    // the dialog shell adds no extra padding.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    content_ = new StatsContentWidget(instance_root, cache_dir, total_mods, this);
    layout->addWidget(content_);

    connect(content_, &StatsContentWidget::close_requested, this,
            &QDialog::accept);
}

} // namespace ui
