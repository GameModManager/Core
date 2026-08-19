#pragma once

#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <filesystem>
#include <string>

namespace ui {

// Read-only instance statistics (instance/mods/cache/downloads/logs sizes,
// total mod count) plus an "Open in size explorer" action. Mode agnostic:
// embedded in a QDialog (InstanceStatisticsDialog) or as a tab page
// (TabModeController::route_stats). Refreshes on every show, so tab
// activation re-reads the current sizes.
class StatsContentWidget : public QWidget {
    Q_OBJECT
public:
    explicit StatsContentWidget(const std::filesystem::path& instance_root,
                                const std::filesystem::path& cache_dir,
                                int total_mods,
                                QWidget* parent = nullptr);

    // Recomputes directory sizes, updates the labels and rewrites
    // statistics.ini in the cache dir.
    void refresh();

signals:
    // Emitted when the user clicks the Close button. The container decides
    // what closing means: QDialog::accept() in popup mode, close_tab() in
    // Full UI tab mode.
    void close_requested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    static uint64_t dir_size(const std::filesystem::path& dir);
    static std::string format_size(uint64_t bytes);

    std::filesystem::path instance_root_;
    std::filesystem::path cache_dir_;
    int total_mods_ = 0;

    QLabel* instance_size_label_ = nullptr;
    QLabel* mods_size_label_ = nullptr;
    QLabel* cache_size_label_ = nullptr;
    QLabel* downloads_size_label_ = nullptr;
    QLabel* logs_size_label_ = nullptr;
    QLabel* total_mods_label_ = nullptr;
    QPushButton* explorer_btn_ = nullptr;
};

} // namespace ui
