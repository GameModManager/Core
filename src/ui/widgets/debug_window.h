#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTimer;

namespace engine {
class GameKnowledge;
class Instance;
class PluginLoader;
class InstanceRegistry;
namespace profile {
class ProfileManager;
}
} // namespace engine

namespace ui {

class RollingChartWidget;

// Debug panel: a 3-tab QDialog surfacing live process stats (Charts), every
// per-instance filesystem path the engine knows about (Paths), and every
// piece of metadata that would otherwise be invisible (Info). Replaces the
// pre-overhaul 4-row label panel with rolling 60-second charts and scrollable
// key-value tables that copy via Ctrl+C or right-click context menu.
class DebugWindow : public QDialog {
  Q_OBJECT
public:
  explicit DebugWindow(const std::filesystem::path &instance_root,
                       const std::string &game_id, const std::string &game_name,
                       engine::PluginLoader *plugin_loader,
                       std::function<void()> on_reload_ui = nullptr,
                       QWidget *parent = nullptr);
  ~DebugWindow() override;

  // Late injection from MainWindow. Some metadata only resolves after the
  // instance + profile are fully loaded (InstanceRegistry entries, the
  // active profile, ...); the SettingsController calls these right after
  // construction so we don't have to expand the constructor signature.
  void set_instance_registry(engine::InstanceRegistry *registry) {
    instance_registry_ = registry;
    populate_info();
  }
  void set_game_knowledge(engine::GameKnowledge *knowledge) {
    knowledge_ = knowledge;
    populate_info();
  }
  void set_active_profile(engine::profile::ProfileManager *profile) {
    active_profile_ = profile;
    populate_info();
  }
  // Late-bind the current Instance object so the Paths tab can show
  // effective per-folder overrides (mods_dir override, downloads_dir
  // override, ...) in addition to the defaults.
  void set_current_instance(const engine::Instance *inst) {
    current_instance_ = inst;
    populate_paths();
  }

private:
  // Builds the 3 tabs. Called from the constructor; never again.
  void setup_charts_tab();
  void setup_paths_tab();
  void setup_info_tab();

  // Refresh the label group (CPU%/RAM/MiB/disk/uptime). Runs on
  // refresh_timer_ (default 2 s, user-tunable). Also pushes to charts.
  void refresh_stats();

  // Refresh only the chart series; runs at 1 Hz on chart_timer_, decoupled
  // from the label interval.
  void refresh_charts();

  // Rebuild the Paths table (instance + game + directories + config).
  void populate_paths();

  // Rebuild the Info table (instance + registry + categories + profile +
  // deploy + game knowledge + app).
  void populate_info();

  // Append a row to a QTableWidget with key + value (monospace). When
  // `copyable` is true, the value gets a tooltip + TextSelectableByMouse +
  // double-click-to-copy handler.
  void add_kv_row(QTableWidget *table, const QString &key, const QString &value,
                  bool copyable, bool monospace_value = true);

  // Insert a non-selectable group separator row.
  void add_group_header(QTableWidget *table, const QString &label);

  // --- procfs helpers (Linux-only; on other platforms readers return 0/empty)
  // ---
  static std::string read_proc(const char *path);
  static std::string read_proc_line(const char *path, const char *prefix);

  // Returns the field-th whitespace-separated field in /proc/self/stat
  // after the closing ')' (field 2 is "(comm)" with spaces).
  static unsigned long parse_after(const std::string &s, int field_index);

  // --- UI ---
  QTabWidget *tabs_ = nullptr;
  QTableWidget *paths_table_ = nullptr;
  QTableWidget *info_table_ = nullptr;

  // Charts
  RollingChartWidget *cpu_chart_ = nullptr;
  RollingChartWidget *ram_chart_ = nullptr;
  RollingChartWidget *heap_chart_ = nullptr;
  RollingChartWidget *disk_chart_ = nullptr;
  RollingChartWidget *net_chart_ = nullptr;
  RollingChartWidget *jitter_chart_ = nullptr;

  // Header labels (compact summary above each chart): e.g. "12% 3.5/4 cores",
  // "Pss 350 MiB  RSS 412 MiB". Updated on every refresh_stats tick.
  QLabel *cpu_header_ = nullptr;
  QLabel *ram_header_ = nullptr;
  QLabel *heap_header_ = nullptr;
  QLabel *disk_header_ = nullptr;
  QLabel *net_header_ = nullptr;
  QLabel *jitter_header_ = nullptr;

  // Legacy labels (kept so existing qss rules targeting objectNames continue
  // to work; value column is updated every refresh_stats).
  QLabel *cpu_label_ = nullptr;
  QLabel *ram_label_ = nullptr;
  QLabel *disk_label_ = nullptr;
  QLabel *uptime_label_ = nullptr;

  QLabel *interval_label_ = nullptr;
  QPushButton *reload_ui_btn_ = nullptr;

  QTimer *refresh_timer_ = nullptr;
  QTimer *chart_timer_ = nullptr;
  int refresh_interval_ = 2; // seconds for labels; charts always 1 Hz

  // --- Persistent state for delta-based metrics ---
  bool first_cpu_ = true;
  unsigned long prev_proc_ticks_ = 0;
  unsigned long prev_sys_total_ = 0;
  bool first_io_ = true;
  unsigned long long prev_read_bytes_ = 0;
  unsigned long long prev_write_bytes_ = 0;
  bool first_net_ = true;
  unsigned long long prev_rx_bytes_ = 0;
  unsigned long long prev_tx_bytes_ = 0;
  QElapsedTimer jitter_timer_;
  bool first_jitter_ = true;

  // --- Constructor-time args / late-bound pointers ---
  std::filesystem::path instance_root_;
  std::string game_id_;
  std::string game_name_;
  engine::PluginLoader *plugin_loader_ = nullptr;
  engine::InstanceRegistry *instance_registry_ = nullptr;
  engine::GameKnowledge *knowledge_ = nullptr;
  engine::profile::ProfileManager *active_profile_ = nullptr;
  const engine::Instance *current_instance_ = nullptr;
};

} // namespace ui