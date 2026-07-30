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
    void clear_executables();
    [[nodiscard]] QString current_executable() const;
    [[nodiscard]] int current_executable_index() const;
    [[nodiscard]] QStringList executable_paths() const;
    void add_executable(const QString& display_name, const QString& rel_path, const QIcon& icon = {});

signals:
    void run_clicked();
    void shortcut_to_toolbar();
    void shortcut_to_desktop();
    void select_executable_requested();

private:
    QComboBox* exec_combo_ = nullptr;
    QToolButton* run_btn_ = nullptr;
    QToolButton* shortcut_btn_ = nullptr;
};

}  // namespace ui
