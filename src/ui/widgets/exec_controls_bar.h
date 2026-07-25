#pragma once

#include <QWidget>

class QComboBox;
class QToolButton;

namespace ui {

class ExecControlsBar : public QWidget {
    Q_OBJECT
public:
    explicit ExecControlsBar(QWidget* parent = nullptr);

    [[nodiscard]] QString current_executable() const;

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
