#pragma once

#include <QString>
#include <QWidget>
#include <filesystem>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace engine {
class PluginLoader;
}

namespace ui {

struct InstanceSwitcherEntry {
  std::string name;              // folder name (e.g. "The_Binding_of_Isaac_Rebirth")
  std::string game_id;           // from instance.toml
  std::string display_name;      // resolved from the plugin (fallback: game_id)
  std::filesystem::path root;    // absolute path to instance root
  bool portable      = true;     // from instance.toml (default: portable)
  QLabel *icon_label = nullptr;  // owned by the list widget; refreshed when
                                 // the async icon fetch lands
};

// Mode-agnostic instance switcher. Extracted from InstanceSwitcherDialog so
// the same content can be embedded either in a popup QDialog
// (InstanceSwitcherDialog) or as a tab page inside MainTabContainer (Full UI
// tab mode).
//
// The widget never switches anything by itself: activating an instance row
// emits instance_selected() and the host decides what switching means
// (QDialog::accept() in popup mode; switch_to_instance() + closing the tab in
// Full UI tab mode). The create button emits create_new_instance() and the
// host runs the GameSelectionWidget create flow.
class InstanceSwitcherContentWidget : public QWidget {
  Q_OBJECT
public:
  explicit InstanceSwitcherContentWidget(engine::PluginLoader *plugins,
                                         QWidget *parent = nullptr);

  // Populate from the global instances directory
  void load_instances(const std::string &instances_dir);

  // Name of the currently selected instance (empty when nothing selected).
  [[nodiscard]] QString selected_instance() const;

  // When enabled (Full UI tab mode), a single click on an instance row
  // emits instance_selected() immediately. Disabled by default so popup
  // mode keeps OK / double-click semantics.
  void set_immediate_switch(bool enabled);

  // Active instance root (used to mark the current row and guard deletion).
  void set_active_instance(const std::filesystem::path &root);

  // Action handlers (also wired to buttons).
  void delete_selected();
  void rename_selected();
  void duplicate_selected();
  void open_folder_selected();
  void open_toml_selected();

  // Filter text.
  void set_filter_text(const QString &text);

signals:
  // Emitted when the user activates an instance row (double-click always;
  // single click when immediate switch is enabled). The host decides what
  // switching means.
  void instance_selected(const QString &name);

  // Emitted when the user clicks "Create new instance". The host runs the
  // GameSelectionWidget create flow.
  void create_new_instance();

  // Emitted after an instance is deleted/renamed/duplicated so hosts can
  // refresh menus/lists.
  void instances_changed();

private:
  void refresh_list();
  void update_icons_for(const QString &game_id);
  void emit_selected();
  void update_details();
  void apply_filter();

  engine::PluginLoader *plugins_ = nullptr;
  std::string instances_dir_;
  std::filesystem::path active_root_;
  QListWidget *list_             = nullptr;
  QLineEdit *filter_             = nullptr;
  QWidget *details_              = nullptr;
  QLabel *details_empty_         = nullptr;
  QWidget *details_form_         = nullptr;
  QLabel *detail_name_value_     = nullptr;
  QLabel *detail_location_value_ = nullptr;
  QLabel *detail_game_value_     = nullptr;
  QLabel *detail_game_dir_value_ = nullptr;
  bool immediate_switch_         = false;
  std::vector<InstanceSwitcherEntry> entries_;
  QString filter_text_;
};

}  // namespace ui