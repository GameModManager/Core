#pragma once

#include <QDialog>
#include <filesystem>

namespace ui {

class StatsContentWidget;

// Thin QDialog wrapper around StatsContentWidget. The stats content itself
// (sizes, explorer button, close button) lives in StatsContentWidget so the
// same widget can be embedded as a tab page in Full UI tab mode.
class InstanceStatisticsDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstanceStatisticsDialog(const std::filesystem::path& instance_root,
                                      const std::filesystem::path& cache_dir,
                                      int total_mods,
                                      QWidget* parent = nullptr);

private:
    StatsContentWidget* content_ = nullptr;
};

} // namespace ui
