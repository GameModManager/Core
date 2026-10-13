#pragma once

#include <QToolButton>
#include <QWidget>

class QBoxLayout;
class QFrame;
class QIcon;
class QMenu;
class QString;

namespace ui {

// One pinned executable shortcut on the toolbar.
struct ExecShortcut {
  QString path;   // game-relative binary path
  QString title;  // entry label, also the button tooltip
  QIcon icon;     // the icon already resolved for the button
};

class MainToolbar : public QWidget {
  Q_OBJECT
public:
  explicit MainToolbar(QWidget *parent = nullptr);

  QToolButton *add_gmm_button(const QString &tooltip, const QString &icon_name);
  QToolButton *add_exec_button(const QString &tooltip, const QIcon &icon);
  void remove_exec_button(QToolButton *btn);
  void clear_exec_buttons();

  // Instance Options button, a split control: the body emits
  // instance_options_clicked() and opens the Instance Options panel, the side
  // arrow opens the menu handed to set_instance_options_menu. Both halves stay
  // live at all times - attaching or detaching the menu never changes the
  // popup mode, because InstantPopup would stop the body half from firing.
  QToolButton *add_instance_options_button(const QIcon &icon);
  void set_instance_options_menu(QMenu *menu);

  void set_vertical(bool vertical);
  void set_icon_size(int size);

  // The pinned executable shortcuts in toolbar order, with the path, title and
  // icon the buttons already carry. Feeds the Run menu.
  [[nodiscard]] QList<ExecShortcut> exec_shortcuts() const;

  // Re-resolve the built-in buttons (Switch Instance, Settings, Instance
  // Options) through IconManager after the icon-pack setting changes.
  void reapply_icons();

  [[nodiscard]] bool is_vertical() const { return vertical_; }
  [[nodiscard]] int current_icon_size() const { return current_icon_size_; }

signals:
  void settings_clicked();
  void instances_clicked();
  void instance_options_clicked();
  void check_updates_clicked();
  void shortcut_removed(const QString &path);

private:
  bool vertical_         = false;
  int current_icon_size_ = 24;  // last icon size applied via set_icon_size
  QBoxLayout *layout_    = nullptr;
  QFrame *separator_     = nullptr;
  QToolButton *instance_options_button_ = nullptr;
  QList<QToolButton *> gmm_buttons_;
  QList<QToolButton *> exec_buttons_;
};

}  // namespace ui
