#pragma once

#include <QWidget>
#include <QStringList>
#include <filesystem>

class QComboBox;
class QToolButton;

namespace ui {

class ExecControlsBar : public QWidget {
    Q_OBJECT
public:
    explicit ExecControlsBar(QWidget* parent = nullptr);

    void set_executables(const QStringList& names, const QString& default_name = {},
                         const std::filesystem::path& game_dir = {},
                         const std::filesystem::path& icon_cache_dir = {});
    [[nodiscard]] QString current_executable() const;
    [[nodiscard]] int current_executable_index() const;

signals:
    void run_clicked();
    void shortcut_to_toolbar();
    void shortcut_to_desktop();

private:
    QComboBox* exec_combo_ = nullptr;
    QToolButton* run_btn_ = nullptr;
    QToolButton* shortcut_btn_ = nullptr;
};

}  // namespace ui
