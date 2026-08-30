#pragma once

#include <QList>
#include <QString>
#include <functional>

namespace ui {

class MainWindow;

// Mod::Actions — extracted action methods for mod operations (remove, move,
// toggle, create separator, rename, color, etc.).
//
// Each action operates on the model via MainWindow's mod_model_ and mod_view_.
// Callbacks are provided for cross-controller calls that stay in
// ModListController.
class ModActions {
public:
  explicit ModActions(MainWindow *w);

  // Set callbacks for cross-controller operations that remain in
  // ModListController.
  void set_sync_mod_enable_state(std::function<void(const QString &, bool)> cb);
  void set_refresh_data_tab(std::function<void()> cb);
  void set_apply_mod_filter(std::function<void()> cb);
  void set_load_mods_from_game(std::function<void()> cb);

  // Context-menu actions.
  void remove_selected_mods();
  void move_to_separator(const QString &mod_id, const QString &sep_id);
  void send_to_separator(const QString &mod_id);
  void send_to_highest_priority(const QString &id);
  void send_to_lowest_priority(const QString &id);
  void send_to_highest_in_separator(const QString &id);
  void send_to_lowest_in_separator(const QString &id);
  void priority_move_selected(int step);
  void toggle_selected_mods(bool enabled);
  void toggle_root_override(const QList<int> &rows, bool on);

  // Separator creation.
  void create_separator_at_row(int row);
  QString create_separator_named(const QString &name, const QString &color);
  void create_separator();
  void create_empty_mod();

  // Rename.
  void rename_mod_inline(int row);
  void apply_rename(int row, const QString &name);
  void delete_separator(int row);

  // Color.
  void select_color_for_selected();
  void reset_color_for_selected();

private:
  MainWindow *w_ = nullptr;

  // Callbacks for cross-controller operations.
  std::function<void(const QString &, bool)> sync_mod_enable_state_cb_;
  std::function<void()> refresh_data_tab_cb_;
  std::function<void()> apply_mod_filter_cb_;
  std::function<void()> load_mods_from_game_cb_;
};

} // namespace ui
