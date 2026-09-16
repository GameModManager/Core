#pragma once

#include <QElapsedTimer>
#include <QHideEvent>
#include <QMainWindow>
#include <QShowEvent>
#include <QString>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QDockWidget;
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

// Debug panel: a QMainWindow hosting one QDockWidget per diagnostics section
// (Charts, Paths, Info, Network, Memory, Modpack). Docks can be rearranged,
// floated, tabified, or shown side-by-side; the default layout tabifies them
// all in one dock area. Surfaces live process stats (Charts), every
// per-instance filesystem path the engine knows about (Paths), otherwise
// invisible metadata (Info), the network request log (Network), a deeply
// analytical memory breakdown (Memory), and modpack/collection install data
// (Modpack).
class DebugWindow : public QMainWindow {
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
    refresh_populated();
  }
  void set_game_knowledge(engine::GameKnowledge *knowledge) {
    knowledge_ = knowledge;
    refresh_populated();
  }
  void set_active_profile(engine::profile::ProfileManager *profile) {
    active_profile_ = profile;
    refresh_populated();
  }
  // Late-bind the current Instance object so the Paths tab can show
  // effective per-folder overrides (mods_dir override, downloads_dir
  // override, ...) in addition to the defaults. Tracks the instance root
  // so a showEvent() can detect an instance switch and repopulate.
  void set_current_instance(const engine::Instance *inst) {
    current_instance_ = inst;
    refresh_populated();
  }
  // Convenience used by MainWindow::switch_to_instance() to re-bind the
  // new instance/game/profile in one shot. Updates the cached root + game
  // IDs and refreshes both tables if the window is currently visible
  // (so the next show is in sync). The constructor already calls the
  // per-tab populators, so callers don't need to chain them.
  void rebind_for_instance(const std::filesystem::path &instance_root,
                           const std::string &game_id,
                           const std::string &game_name) {
    instance_root_ = instance_root;
    game_id_ = game_id;
    game_name_ = game_name;
    refresh_populated();
  }

  // Toggles visibility (Help > Debug Panel wiring).
  void toggle_visible() {
    if (isVisible()) {
      hide();
    } else {
      show();
      raise();
      activateWindow();
    }
  }

private:
  // Builds each page's content widget. Called once from the constructor;
  // the widgets are then wrapped in QDockWidgets.
  QWidget *build_charts_page();
  QWidget *build_paths_page();
  QWidget *build_info_page();
  QWidget *build_network_page();
  QWidget *build_memory_page();
  QWidget *build_modpack_page();

  // Wraps a page widget in a dockable panel and docks it.
  QDockWidget *make_dock(const QString &title, QWidget *content);

  // Restores the default tabified layout and re-shows every dock.
  void reset_dock_layout();

  // Re-runs the tab populators. Called whenever any of the late-bound
  // inputs change (registry, knowledge, profile, current Instance) and
  // from rebind_for_instance() when MainWindow::set_game_info() runs.
  void refresh_populated();

  // Safety net: if the cached instance root diverges from the one the
  // caller pushed via rebind_for_instance(), repopulate on show.
  void showEvent(QShowEvent *event) override;
  // Pause the periodic refresh while the dialog is hidden so the table
  // rebuild doesn't burn CPU on a panel nobody is looking at.
  void hideEvent(QHideEvent *event) override;

  // Refresh the label group (CPU%/RAM/MiB/disk/uptime). Runs on
  // refresh_timer_ at a fixed 1 s interval. Also pushes to charts.
  void refresh_stats();

  // Refresh only the chart series; runs at 1 Hz on chart_timer_, decoupled
  // from the label interval.
  void refresh_charts();

  // Rebuild the Paths table (instance + game + directories + config).
  void populate_paths();

  // Rebuild the Info table (instance + registry + categories + profile +
  // deploy + game knowledge + app).
  void populate_info();

  // Rebuild the Network log table from engine::network::log_snapshot().
  // Called whenever refresh_stats runs (every refresh_interval seconds).
  void populate_network();

  // Rebuild the Memory page tables (stats + subsystem inventory +
  // per-type counters + allocation tracker).
  void populate_memory();
  void populate_modpack();

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

  // --- Docks (one per page; tabified by default) ---
  QDockWidget *charts_dock_ = nullptr;
  QDockWidget *paths_dock_ = nullptr;
  QDockWidget *info_dock_ = nullptr;
  QDockWidget *network_dock_ = nullptr;
  QDockWidget *memory_dock_ = nullptr;
  QDockWidget *modpack_dock_ = nullptr;

  // --- Page content ---
  QTableWidget *paths_table_ = nullptr;
  QTableWidget *info_table_ = nullptr;
  QTableWidget *network_table_ = nullptr;

  // Track the last (newest) log id we rendered so we can detect "no new
  // entries" and skip the table rebuild entirely. Keeps the UI idle
  // when there's no traffic.
  std::uint64_t last_network_log_id_ = 0;

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

  QPushButton *reload_ui_btn_ = nullptr;

  // --- Memory page widgets ---
  QTableWidget *mem_stats_table_ = nullptr;
  QTableWidget *mem_subsys_table_ = nullptr;
  QTableWidget *mem_types_table_ = nullptr;
  QTableWidget *mem_alloc_table_ = nullptr;

  // --- Modpack page widgets ---
  QLabel *modpack_status_ = nullptr;
  QTableWidget *modpack_table_ = nullptr;

  QTimer *refresh_timer_ = nullptr;
  QTimer *chart_timer_ = nullptr;

  // --- Persistent state for delta-based metrics ---
  // refresh_charts() runs at 1 Hz (chart_timer_); refresh_stats() runs on
  // refresh_timer_ at a fixed 1 s interval. The two
  // paths used to share these prev_* members and so each timer would
  // clobber the other's baseline: label deltas could be computed against
  // a baseline that the chart path had just rewritten. Splitting them
  // gives each path its own baseline and removes the race.
  bool first_cpu_chart_ = true;
  unsigned long prev_proc_ticks_chart_ = 0;
  unsigned long prev_sys_total_chart_ = 0;
  bool first_io_chart_ = true;
  unsigned long long prev_read_bytes_chart_ = 0;
  unsigned long long prev_write_bytes_chart_ = 0;
  bool first_net_chart_ = true;
  unsigned long long prev_rx_bytes_chart_ = 0;
  unsigned long long prev_tx_bytes_chart_ = 0;
  QElapsedTimer jitter_timer_;
  bool first_jitter_ = true;

  bool first_cpu_label_ = true;
  unsigned long prev_proc_ticks_label_ = 0;
  unsigned long prev_sys_total_label_ = 0;
  bool first_io_label_ = true;
  unsigned long long prev_read_bytes_label_ = 0;
  unsigned long long prev_write_bytes_label_ = 0;

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
