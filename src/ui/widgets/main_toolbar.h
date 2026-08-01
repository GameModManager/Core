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
    QToolButton* add_exec_button(const QString& tooltip, const QIcon& icon);
    void remove_exec_button(QToolButton* btn);
    void clear_exec_buttons();

    void set_vertical(bool vertical);
    void set_icon_size(int size);
    [[nodiscard]] bool is_vertical() const { return vertical_; }

signals:
    void settings_clicked();
    void instances_clicked();
    void shortcut_removed(const QString& path);

private:
    bool vertical_ = false;
    QBoxLayout* layout_ = nullptr;
    QFrame* separator_ = nullptr;
    QList<QToolButton*> gmm_buttons_;
    QList<QToolButton*> exec_buttons_;
};

}  // namespace ui
