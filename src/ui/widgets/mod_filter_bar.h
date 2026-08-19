#pragma once

#include <QWidget>

class QComboBox;
class QLineEdit;
class QToolButton;

namespace ui {

class ModFilterBar : public QWidget {
    Q_OBJECT
public:
    explicit ModFilterBar(QWidget* parent = nullptr);

    [[nodiscard]] QString filter_text() const;
    [[nodiscard]] QString current_group() const;

signals:
    void filter_changed(const QString& text);
    void group_changed(const QString& group);
    // Emitted when the << / >> category-panel toggle is clicked; `visible` is
    // the new panel state (true = panel shown).
    void category_panel_toggled(bool visible);

private:
    QToolButton* category_toggle_btn_ = nullptr;
    QComboBox* group_combo_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
};

}  // namespace ui
