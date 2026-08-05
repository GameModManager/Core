#pragma once

#include <QToolButton>
#include <QWidget>

class QBoxLayout;
class QFrame;
class QMenu;

namespace ui {

class MainToolbar : public QWidget {
    Q_OBJECT
public:
    explicit MainToolbar(QWidget* parent = nullptr);

    QToolButton* add_gmm_button(const QString& tooltip, const QString& icon_name);
    QToolButton* add_exec_button(const QString& tooltip, const QIcon& icon);
    void remove_exec_button(QToolButton* btn);
    void clear_exec_buttons();

    // Proton button: body click emits proton_clicked() (opens the Proton
    // panel); the attached dropdown arrow shows `menu` (set via set_proton_menu).
    QToolButton* add_proton_button(const QIcon& icon);
    void set_proton_menu(QMenu* menu);

    void set_vertical(bool vertical);
    void set_icon_size(int size);

    // Re-resolve the built-in buttons (Switch Instance, Settings, Proton)
    // through IconManager after the icon-pack setting changes.
    void reapply_icons();

    [[nodiscard]] bool is_vertical() const { return vertical_; }
    [[nodiscard]] int current_icon_size() const { return current_icon_size_; }

signals:
    void settings_clicked();
    void instances_clicked();
    void proton_clicked();
    void shortcut_removed(const QString& path);

private:
    bool vertical_ = false;
    int current_icon_size_ = 24;  // last icon size applied via set_icon_size
    QBoxLayout* layout_ = nullptr;
    QFrame* separator_ = nullptr;
    QToolButton* proton_button_ = nullptr;
    QList<QToolButton*> gmm_buttons_;
    QList<QToolButton*> exec_buttons_;
};

}  // namespace ui
