#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <filesystem>
#include <string>

namespace ui {

class InstanceStatisticsDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstanceStatisticsDialog(const std::filesystem::path& instance_root,
                                      const std::filesystem::path& cache_dir,
                                      int total_mods,
                                      QWidget* parent = nullptr);

private:
    static uint64_t dir_size(const std::filesystem::path& dir);
    static std::string format_size(uint64_t bytes);

    QLabel* instance_size_label_ = nullptr;
    QLabel* mods_size_label_ = nullptr;
    QLabel* cache_size_label_ = nullptr;
    QLabel* downloads_size_label_ = nullptr;
    QLabel* logs_size_label_ = nullptr;
    QLabel* total_mods_label_ = nullptr;
    QPushButton* explorer_btn_ = nullptr;
    std::filesystem::path instance_root_;
};

} // namespace ui
