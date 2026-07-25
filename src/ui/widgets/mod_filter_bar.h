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
    void expand_all_clicked();
    void collapse_all_clicked();

private:
    QToolButton* expand_btn_ = nullptr;
    QComboBox* group_combo_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
};

}  // namespace ui
