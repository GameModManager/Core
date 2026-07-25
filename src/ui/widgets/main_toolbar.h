#pragma once

#include <QToolButton>
#include <QWidget>

class QBoxLayout;
class QFrame;

namespace ui {

class MainToolbar : public QWidget {
    Q_OBJECT
public:
    explicit MainToolbar(QWidget* parent = nullptr);

    QToolButton* add_gmm_button(const QString& tooltip, const QString& icon_name);
    QToolButton* add_exec_button(const QString& tooltip, const QString& icon_name);

    void set_vertical(bool vertical);
    [[nodiscard]] bool is_vertical() const { return vertical_; }

signals:
    void settings_clicked();
    void profiles_clicked();
    void instances_clicked();

private:
    bool vertical_ = false;
    QBoxLayout* layout_ = nullptr;
    QFrame* separator_ = nullptr;
    QList<QToolButton*> gmm_buttons_;
    QList<QToolButton*> exec_buttons_;
};

}  // namespace ui
