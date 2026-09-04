#include "ui/widgets/debug_window.h"

#include "engine/core/instance/instance.h"
#include "engine/core/instance/instance_registry.h"
#include "engine/core/instance/instance_utils.h"
#include "engine/game/registry/game_knowledge.h"
#include "engine/network/network_manager.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/profile/profile.h"
#include "ui/widgets/rolling_chart.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QSysInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef VERSION
#define VERSION "unknown"
#endif

namespace fs = std::filesystem;

namespace ui {

namespace {

// --- procfs value parsers --------------------------------------------------

// Parse "Key: <value> kB" (or "Key: <value>") from /proc/self/smaps_rollup
// and /proc/self/status. Returns 0 on miss/parse error (raw unsigned long).
unsigned long parse_kb_line(const std::string &blob, const char *key) {
  auto pos = blob.find(key);
  if (pos == std::string::npos)
    return 0;
  pos += std::strlen(key);
  while (pos < blob.size() && (blob[pos] == ' ' || blob[pos] == '\t'))
    ++pos;
  auto end = blob.find('\n', pos);
  if (end == std::string::npos)
    end = blob.size();
  auto val_str = blob.substr(pos, end - pos);
  auto space = val_str.find(' ');
  if (space != std::string::npos)
    val_str = val_str.substr(0, space);
  // stoul throws on bad parse - guard.
  try {
    return std::stoul(val_str);
  } catch (...) {
    return 0;
  }
}

// Parse "Key: <value>" from /proc/self/io returning an unsigned long long
// (no kB suffix on io counters; raw bytes).
unsigned long long parse_io_value(const std::string &blob, const char *key) {
  auto pos = blob.find(key);
  if (pos == std::string::npos)
    return 0;
  pos += std::strlen(key);
  while (pos < blob.size() && (blob[pos] == ' ' || blob[pos] == '\t'))
    ++pos;
  auto end = blob.find('\n', pos);
  if (end == std::string::npos)
    end = blob.size();
  auto val_str = blob.substr(pos, end - pos);
  auto space = val_str.find(' ');
  if (space != std::string::npos)
    val_str = val_str.substr(0, space);
  unsigned long long result = 0;
  std::from_chars(val_str.data(), val_str.data() + val_str.size(), result);
  return result;
}

// CPU cores (online); 0 when sysconf fails (non-Linux).
long online_cpu_count() {
#ifdef __linux__
  long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? n : 1;
#else
  return 0;
#endif
}

// CPU model name from /proc/cpuinfo (first "model name" line). Empty when
// not Linux or the field is missing (rare ARM boards).
std::string cpu_model_name() {
#ifdef __linux__
  std::ifstream f("/proc/cpuinfo");
  if (!f)
    return {};
  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 10, "model name") == 0) {
      auto colon = line.find(':');
      if (colon == std::string::npos)
        return {};
      auto val = line.substr(colon + 1);
      // Trim leading whitespace.
      auto first = val.find_first_not_of(" \t");
      if (first == std::string::npos)
        return {};
      return val.substr(first);
    }
  }
#endif
  return {};
}

// Sum of rx_bytes (column 1) and tx_bytes (column 9) across all interfaces
// in /proc/net/dev except lo. Returns {rx, tx}; zero on non-Linux.
struct NetCounters {
  unsigned long long rx = 0;
  unsigned long long tx = 0;
};
NetCounters sum_net_counters() {
  NetCounters out;
#ifdef __linux__
  std::ifstream f("/proc/net/dev");
  if (!f)
    return out;
  std::string line;
  // Skip the two header lines.
  std::getline(f, line);
  std::getline(f, line);
  while (std::getline(f, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    // /proc/net/dev lines are indented ("    lo: ..."), so trim leading
    // whitespace before the comparison - without this "lo" never matches
    // and the loopback counters inflate the system total.
    std::string iface = line.substr(0, colon);
    auto first = iface.find_first_not_of(" \t");
    if (first == std::string::npos)
      continue;
    iface = iface.substr(first);
    if (iface == "lo")
      continue;
    // 16 columns on Linux: name rx_bytes rx_packets ... rx_errs rx_drop
    // rx_fifo rx_frame rx_compressed rx_multicast tx_bytes ...
    // We need columns 1 (rx_bytes) and 9 (tx_bytes) when the interface
    // name is at column 0.
    unsigned long long rx = 0, tx = 0;
    std::istringstream iss(line.substr(colon + 1));
    int col = 0;
    unsigned long long v;
    while (iss >> v) {
      if (col == 0)
        rx = v;
      if (col == 8)
        tx = v;
      ++col;
    }
    out.rx += rx;
    out.tx += tx;
  }
  // Note: the sum above is system-wide and includes docker0 / veth* / br-*
  // noise from any containers/netns. For per-process byte counters on
  // Linux use /proc/<pid>/net/dev (out of scope here). On non-Linux
  // platforms this returns zero and the chart shows "(unsupported)".
#endif
  return out;
}

QString bool_text(bool v) {
  return v ? QStringLiteral("true") : QStringLiteral("false");
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DebugWindow::DebugWindow(const fs::path &instance_root,
                         const std::string &game_id,
                         const std::string &game_name,
                         engine::PluginLoader *plugin_loader,
                         std::function<void()> on_reload_ui, QWidget *parent)
    : QDialog(parent, Qt::Window | Qt::WindowStaysOnTopHint),
      instance_root_(instance_root), game_id_(game_id), game_name_(game_name),
      plugin_loader_(plugin_loader) {

  setWindowTitle(tr("Debug Panel"));
  setMinimumSize(720, 540);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(6, 6, 6, 6);
  root->setSpacing(6);

  tabs_ = new QTabWidget(this);
  setup_charts_tab();
  setup_paths_tab();
  setup_info_tab();
  setup_network_tab();
  root->addWidget(tabs_, 1);

  // --- Interval controls (kept; only affects the legacy labels now) ---
  auto *interval_row = new QHBoxLayout;
  auto *dec_btn = new QPushButton("-");
  dec_btn->setFixedWidth(28);
  interval_label_ = new QLabel(QString::number(refresh_interval_));
  interval_label_->setObjectName("debugValue");
  interval_label_->setAlignment(Qt::AlignCenter);
  interval_label_->setFixedWidth(30);
  auto *inc_btn = new QPushButton("+");
  inc_btn->setFixedWidth(28);
  auto *sec_label = new QLabel(tr("label refresh (s)"));
  sec_label->setObjectName("debugKey");
  interval_row->addWidget(dec_btn);
  interval_row->addWidget(interval_label_);
  interval_row->addWidget(inc_btn);
  interval_row->addWidget(sec_label);
  interval_row->addStretch();
  root->addLayout(interval_row);

  connect(dec_btn, &QPushButton::clicked, this, [this]() {
    if (refresh_interval_ > 1) {
      refresh_interval_ -= 1;
      interval_label_->setText(QString::number(refresh_interval_));
      if (refresh_timer_)
        refresh_timer_->setInterval(refresh_interval_ * 1000);
    }
  });
  connect(inc_btn, &QPushButton::clicked, this, [this]() {
    if (refresh_interval_ < 60) {
      refresh_interval_ += 1;
      interval_label_->setText(QString::number(refresh_interval_));
      if (refresh_timer_)
        refresh_timer_->setInterval(refresh_interval_ * 1000);
    }
  });

  // --- Button row (Reload UI + Close) ---
  auto *btn_row = new QHBoxLayout;
  btn_row->setAlignment(Qt::AlignCenter);
  btn_row->setSpacing(12);
  reload_ui_btn_ = new QPushButton(tr("Reload UI"));
  reload_ui_btn_->setToolTip(tr("Re-read debug.qss and apply (Ctrl+Shift+R)"));
  if (on_reload_ui) {
    connect(reload_ui_btn_, &QPushButton::clicked, this,
            [on_reload_ui]() { on_reload_ui(); });
  }
  btn_row->addWidget(reload_ui_btn_);
  auto *close_btn = new QPushButton(tr("Close"));
  connect(close_btn, &QPushButton::clicked, this, [this]() { hide(); });
  btn_row->addWidget(close_btn);
  root->addLayout(btn_row);

  // Seed charts/paths/info, start the timers.
  populate_paths();
  populate_info();
  jitter_timer_.start();
  refresh_stats();

  refresh_timer_ = new QTimer(this);
  connect(refresh_timer_, &QTimer::timeout, this, &DebugWindow::refresh_stats);
  refresh_timer_->start(refresh_interval_ * 1000);

  chart_timer_ = new QTimer(this);
  connect(chart_timer_, &QTimer::timeout, this, &DebugWindow::refresh_charts);
  chart_timer_->start(1000);
}

DebugWindow::~DebugWindow() {
  if (refresh_timer_)
    refresh_timer_->stop();
  if (chart_timer_)
    chart_timer_->stop();
}

// ---------------------------------------------------------------------------
// Tab construction
// ---------------------------------------------------------------------------

namespace {

// Wraps a single chart widget (header label + RollingChartWidget) into a
// QGroupBox, so the chart tab lays them out in a grid.
QGroupBox *wrap_chart(const QString &title, QLabel **header_out,
                      RollingChartWidget **chart_out) {
  auto *gb = new QGroupBox(title);
  auto *v = new QVBoxLayout(gb);
  v->setContentsMargins(6, 12, 6, 6);
  v->setSpacing(2);
  auto *hdr = new QLabel(QStringLiteral("-"));
  hdr->setObjectName("debugValue");
  hdr->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  *header_out = hdr;
  v->addWidget(hdr);
  auto *chart = new RollingChartWidget();
  *chart_out = chart;
  v->addWidget(chart, 1);
  return gb;
}

} // namespace

void DebugWindow::setup_charts_tab() {
  auto *tab = new QWidget;
  auto *outer = new QVBoxLayout(tab);
  outer->setContentsMargins(4, 4, 4, 4);

  // Legacy labels stay so existing qss rules targeting objectNames keep
  // working; they're tucked above the charts in a horizontal row.
  auto *legacy_row = new QHBoxLayout;
  auto add_legacy = [&](const char *key, QLabel **out) {
    auto *k = new QLabel(QCoreApplication::translate("DebugWindow", key));
    k->setObjectName("debugKey");
    *out = new QLabel(QStringLiteral("-"));
    (*out)->setObjectName("debugValue");
    (*out)->setAlignment(Qt::AlignRight);
    legacy_row->addWidget(k);
    legacy_row->addWidget(*out, 1);
  };
  add_legacy("CPU:", &cpu_label_);
  add_legacy("RAM:", &ram_label_);
  add_legacy("Disk:", &disk_label_);
  add_legacy("Uptime:", &uptime_label_);
  outer->addLayout(legacy_row);

  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);

  auto *grid_host = new QWidget;
  auto *grid = new QGridLayout(grid_host);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(8);
  grid->setColumnStretch(0, 1);
  grid->setColumnStretch(1, 1);

  grid->addWidget(wrap_chart(tr("CPU"), &cpu_header_, &cpu_chart_), 0, 0);
  grid->addWidget(wrap_chart(tr("RAM (Pss)"), &ram_header_, &ram_chart_), 0, 1);
  grid->addWidget(wrap_chart(tr("Heap (VmData)"), &heap_header_, &heap_chart_),
                  1, 0);
  grid->addWidget(wrap_chart(tr("Disk I/O"), &disk_header_, &disk_chart_), 1,
                  1);
  grid->addWidget(wrap_chart(tr("Network I/O"), &net_header_, &net_chart_), 2,
                  0);
  grid->addWidget(
      wrap_chart(tr("Event-loop jitter"), &jitter_header_, &jitter_chart_), 2,
      1);

  scroll->setWidget(grid_host);
  outer->addWidget(scroll, 1);

  // Configure Y axes / labels.
  cpu_chart_->set_y_range(0.0, 100.0);
  cpu_chart_->set_y_label(QStringLiteral("%"));
  cpu_chart_->set_title(QStringLiteral("%1 cores").arg(online_cpu_count()));
  // RAM and Heap are auto-scaled: a fixed 0..1024 MiB clip hides any
  // process that grows past 1 GiB (very common during long sessions,
  // indexing, or Steam Workshop downloads). Both metrics are clamped
  // to a non-negative lower bound - RSS and VmData cannot go below 0.
  ram_chart_->set_clamp_negative(true);
  ram_chart_->set_y_label(QStringLiteral("MiB"));
  heap_chart_->set_clamp_negative(true);
  heap_chart_->set_y_label(QStringLiteral("MiB"));
  // Disk and Net: split into two series (Read/Write and RX/TX) so the
  // trends are visible independently instead of a single sum that
  // cancels out spikes, and clamp the lower bound to 0 so an idle
  // chart never shows negative KiB/s. Legend labels make the two
  // series visually distinguishable.
  disk_chart_->set_clamp_negative(true);
  disk_chart_->set_y_label(QStringLiteral("KiB/s"));
  disk_chart_->set_legend(QStringLiteral("Read"));
  disk_chart_->set_legend2(QStringLiteral("Write"));
  net_chart_->set_clamp_negative(true);
  net_chart_->set_y_label(QStringLiteral("KiB/s"));
  net_chart_->set_legend(QStringLiteral("RX"));
  net_chart_->set_legend2(QStringLiteral("TX"));
  // jitter: signed -50..+50 ms - must NOT be clamped so the +/- range
  // is preserved (jitter is the only signed metric on this panel).
  jitter_chart_->set_y_range(-50.0, 50.0);
  jitter_chart_->set_y_label(QStringLiteral("ms"));

  tabs_->addTab(tab, tr("Charts"));
}

void DebugWindow::setup_paths_tab() {
  auto *tab = new QWidget;
  auto *v = new QVBoxLayout(tab);
  v->setContentsMargins(4, 4, 4, 4);
  paths_table_ = new QTableWidget(0, 2, tab);
  paths_table_->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
  paths_table_->verticalHeader()->hide();
  paths_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  paths_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  paths_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  paths_table_->setAlternatingRowColors(true);
  paths_table_->setShowGrid(false);
  paths_table_->setWordWrap(false);
  auto *hh = paths_table_->horizontalHeader();
  hh->setStretchLastSection(true);
  hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  // Ctrl+C copies the focused (or selected) row's value.
  paths_table_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(paths_table_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) {
            auto *item = paths_table_->itemAt(pos);
            if (!item)
              return;
            int row = item->row();
            auto *v = paths_table_->item(row, 1);
            if (!v)
              return;
            QGuiApplication::clipboard()->setText(v->text());
          });
  connect(paths_table_, &QAbstractItemView::doubleClicked, this,
          [this](const QModelIndex &idx) {
            if (idx.column() != 1)
              return;
            auto *it = paths_table_->item(idx.row(), 1);
            if (it)
              QGuiApplication::clipboard()->setText(it->text());
          });
  v->addWidget(paths_table_);
  tabs_->addTab(tab, tr("Paths"));
}

void DebugWindow::setup_info_tab() {
  auto *tab = new QWidget;
  auto *v = new QVBoxLayout(tab);
  v->setContentsMargins(4, 4, 4, 4);
  info_table_ = new QTableWidget(0, 2, tab);
  info_table_->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
  info_table_->verticalHeader()->hide();
  info_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  info_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  info_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  info_table_->setAlternatingRowColors(true);
  info_table_->setShowGrid(false);
  auto *hh = info_table_->horizontalHeader();
  hh->setStretchLastSection(true);
  hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  hh->setSectionResizeMode(1, QHeaderView::Stretch);
  info_table_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(info_table_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) {
            auto *item = info_table_->itemAt(pos);
            if (!item)
              return;
            auto *v = info_table_->item(item->row(), 1);
            if (v)
              QGuiApplication::clipboard()->setText(v->text());
          });
  connect(info_table_, &QAbstractItemView::doubleClicked, this,
          [this](const QModelIndex &idx) {
            if (idx.column() != 1)
              return;
            auto *it = info_table_->item(idx.row(), 1);
            if (it)
              QGuiApplication::clipboard()->setText(it->text());
          });
  v->addWidget(info_table_);
  tabs_->addTab(tab, tr("Info"));
}

// ---------------------------------------------------------------------------
// Table helpers
// ---------------------------------------------------------------------------

void DebugWindow::add_kv_row(QTableWidget *table, const QString &key,
                             const QString &value, bool copyable,
                             bool monospace_value) {
  if (!table)
    return;
  int row = table->rowCount();
  table->insertRow(row);
  auto *k = new QTableWidgetItem(key);
  k->setFlags(k->flags() & ~Qt::ItemIsEditable);
  k->setFlags(k->flags() & ~Qt::ItemIsSelectable);
  k->setForeground(table->palette().windowText().color());
  table->setItem(row, 0, k);
  auto *val = new QTableWidgetItem(value);
  val->setFlags(val->flags() & ~Qt::ItemIsEditable);
  if (!copyable)
    val->setFlags(val->flags() & ~Qt::ItemIsSelectable);
  if (copyable) {
    val->setToolTip(value);
    val->setData(Qt::UserRole, value);
  }
  if (monospace_value) {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    val->setFont(f);
  }
  table->setItem(row, 1, val);
}

void DebugWindow::add_group_header(QTableWidget *table, const QString &label) {
  if (!table)
    return;
  int row = table->rowCount();
  table->insertRow(row);
  auto *it = new QTableWidgetItem(label);
  it->setFlags(Qt::NoItemFlags);
  QFont f = it->font();
  f.setBold(true);
  it->setFont(f);
  table->setItem(row, 0, it);
  auto *blank = new QTableWidgetItem;
  blank->setFlags(Qt::NoItemFlags);
  table->setItem(row, 1, blank);
}

// ---------------------------------------------------------------------------
// Paths tab population (~22 rows, grouped)
// ---------------------------------------------------------------------------

void DebugWindow::populate_paths() {
  if (!paths_table_)
    return;
  paths_table_->setRowCount(0);

  // Pull the live Instance::Info (per-folder overrides, game_dir, ...) when
  // available so we can show override badges.
  const engine::Instance::Info *info =
      current_instance_ ? &current_instance_->info() : nullptr;

  // Group: Instance Root
  add_group_header(paths_table_, tr("Instance Root"));
  add_kv_row(paths_table_, tr("Instance root"),
             QString::fromStdString(instance_root_.string()), true);
  add_kv_row(
      paths_table_, tr("Instance TOML"),
      QString::fromStdString((instance_root_ / "instance.toml").string()),
      true);
  add_kv_row(paths_table_, tr("Display name"),
             info ? QString::fromStdString(info->display_name)
                  : QString::fromStdString(game_name_),
             true);
  add_kv_row(paths_table_, tr("Folder name"),
             QString::fromStdString(instance_root_.filename().string()), true);
  if (info) {
    add_kv_row(paths_table_, tr("Portable"), bool_text(info->portable), false);
  } else {
    add_kv_row(paths_table_, tr("Portable"), QStringLiteral("(unknown)"),
               false);
  }

  // Group: Game
  add_group_header(paths_table_, tr("Game"));
  add_kv_row(paths_table_, tr("Game ID"), QString::fromStdString(game_id_),
             true);
  QString game_display;
  if (plugin_loader_) {
    game_display =
        QString::fromStdString(plugin_loader_->display_name_for(game_id_));
  }
  add_kv_row(paths_table_, tr("Game display"),
             game_display.isEmpty() ? QStringLiteral("(unknown)")
                                    : game_display,
             true);
  if (info) {
    add_kv_row(paths_table_, tr("Game dir"),
               info->game_dir.empty()
                   ? QStringLiteral("(not set)")
                   : QString::fromStdString(info->game_dir.string()),
               true);
    add_kv_row(paths_table_, tr("Game mods dir (override)"),
               info->game_mods_dir.empty()
                   ? QStringLiteral("(default)")
                   : QString::fromStdString(info->game_mods_dir.string()),
               true);
    add_kv_row(paths_table_, tr("Steam AppID"),
               info->steam_appid == 0 ? QStringLiteral("(none)")
                                      : QString::number(info->steam_appid),
               false);
  } else {
    add_kv_row(paths_table_, tr("Game dir"), QStringLiteral("(unknown)"), true);
    add_kv_row(paths_table_, tr("Game mods dir (override)"),
               QStringLiteral("(unknown)"), true);
    add_kv_row(paths_table_, tr("Steam AppID"), QStringLiteral("(unknown)"),
               false);
  }

  // Group: Directories (effective). path_for() honors overrides.
  add_group_header(paths_table_, tr("Directories (effective)"));
  auto add_path = [&](const QString &name, engine::InstanceKind kind) {
    QString val;
    if (current_instance_) {
      val = QString::fromStdString(current_instance_->path_for(kind).string());
      // Badge whether this is the default or an override.
      auto override = current_instance_->path_override(kind);
      if (!override.empty()) {
        val += QStringLiteral("  (override)");
      } else {
        val += QStringLiteral("  (default)");
      }
    } else {
      // No Instance available: show the conventional <root>/<subdir> path.
      const char *sub = "";
      switch (kind) {
      case engine::InstanceKind::Mods:
        sub = "mods";
        break;
      case engine::InstanceKind::Downloads:
        sub = "downloads";
        break;
      case engine::InstanceKind::Cache:
        sub = "cache";
        break;
      case engine::InstanceKind::CacheArchives:
        sub = "cache/archives";
        break;
      case engine::InstanceKind::CacheThumbnails:
        sub = "cache/thumbnails";
        break;
      case engine::InstanceKind::Profiles:
        sub = "profiles";
        break;
      case engine::InstanceKind::Overwrite:
        sub = "overwrite";
        break;
      case engine::InstanceKind::Plugins:
        sub = "plugins";
        break;
      case engine::InstanceKind::Logs:
        sub = "logs";
        break;
      case engine::InstanceKind::Config:
        sub = "config";
        break;
      case engine::InstanceKind::Meta:
        sub = "meta";
        break;
      case engine::InstanceKind::Masterlists:
        sub = "masterlists";
        break;
      }
      val = QString::fromStdString((instance_root_ / sub).string());
    }
    add_kv_row(paths_table_, name, val, true);
  };
  add_path(tr("Mods dir"), engine::InstanceKind::Mods);
  add_path(tr("Downloads dir"), engine::InstanceKind::Downloads);
  add_path(tr("Cache dir"), engine::InstanceKind::Cache);
  add_path(tr("Cache/archives"), engine::InstanceKind::CacheArchives);
  add_path(tr("Cache/thumbnails"), engine::InstanceKind::CacheThumbnails);
  add_path(tr("Profiles dir"), engine::InstanceKind::Profiles);
  add_path(tr("Overwrite dir"), engine::InstanceKind::Overwrite);
  add_path(tr("Plugins dir"), engine::InstanceKind::Plugins);
  add_path(tr("Logs dir"), engine::InstanceKind::Logs);
  add_path(tr("Config dir"), engine::InstanceKind::Config);
  add_path(tr("Meta dir"), engine::InstanceKind::Meta);
  add_path(tr("Masterlists dir"), engine::InstanceKind::Masterlists);

  // Group: Config
  add_group_header(paths_table_, tr("Config"));
  if (info) {
    add_kv_row(paths_table_, tr("Proton runner"),
               info->proton_runner.empty()
                   ? tr("(auto)")
                   : QString::fromStdString(info->proton_runner),
               true);
    add_kv_row(paths_table_, tr("Plugins.txt override"),
               info->plugins_txt_path.empty()
                   ? tr("(platform default)")
                   : QString::fromStdString(info->plugins_txt_path.string()),
               true);
    add_kv_row(paths_table_, tr("Deploy strategy (override)"),
               info->deploy_strategy.empty()
                   ? tr("(plugin default)")
                   : QString::fromStdString(info->deploy_strategy),
               false);
    add_kv_row(paths_table_, tr("Last tab"),
               info->last_tab.empty() ? tr("(default)")
                                      : QString::fromStdString(info->last_tab),
               false);
  } else {
    add_kv_row(paths_table_, tr("Proton runner"), tr("(unknown)"), true);
    add_kv_row(paths_table_, tr("Plugins.txt override"), tr("(unknown)"), true);
    add_kv_row(paths_table_, tr("Deploy strategy (override)"), tr("(unknown)"),
               false);
    add_kv_row(paths_table_, tr("Last tab"), tr("(unknown)"), false);
  }
}

// ---------------------------------------------------------------------------
// Info tab population (~25 rows, grouped)
// ---------------------------------------------------------------------------

void DebugWindow::populate_info() {
  if (!info_table_)
    return;
  info_table_->setRowCount(0);

  // Group: Instance metadata (from Instance::Info when available).
  add_group_header(info_table_, tr("Instance metadata"));
  add_kv_row(info_table_, tr("Display name"),
             QString::fromStdString(game_name_), true);
  add_kv_row(info_table_, tr("Folder name"),
             QString::fromStdString(instance_root_.filename().string()), true);
  // Use Instance::Info::portable as the source of truth (matches the Paths
  // tab). The previous filename=="ModOrganizer" heuristic was wrong for any
  // portable instance whose folder was named something else, and could
  // disagree with the Paths tab on the same window.
  bool portable = false;
  if (current_instance_) {
    portable = current_instance_->info().portable;
  } else if (instance_registry_) {
    auto entry = instance_registry_->find_by_root(instance_root_);
    if (entry)
      portable = (entry->type == "portable");
  }
  add_kv_row(info_table_, tr("Portable"), bool_text(portable), false);

  // Group: InstanceRegistry
  add_group_header(info_table_, tr("Registry"));
  int reg_total = 0;
  QString active_name;
  QString entry_type;
  QString created_at;
  QString last_used;
  QString validation = tr("(unknown)");
  // The UI does not currently own an InstanceRegistry instance; if the
  // caller wired one in via set_instance_registry(), use it. Otherwise
  // lazily build a temporary one from the on-disk file (load() is silent
  // on missing files, so this is cheap on a clean install).
  engine::InstanceRegistry local_registry;
  engine::InstanceRegistry *reg = instance_registry_;
  std::unique_ptr<engine::InstanceRegistry> owned_reg;
  if (!reg) {
    owned_reg = std::make_unique<engine::InstanceRegistry>();
    (void)owned_reg->load();
    reg = owned_reg.get();
  }
  if (reg) {
    auto all = reg->all_entries();
    reg_total = static_cast<int>(all.size());
    active_name = QString::fromStdString(reg->active_name());
    auto entry = reg->find_by_root(instance_root_);
    if (entry) {
      entry_type = QString::fromStdString(entry->type);
      created_at = QString::fromStdString(entry->created_at);
      last_used = QString::fromStdString(entry->last_used_at);
      auto status = reg->validate(*entry);
      switch (status) {
      case engine::InstanceRegistry::ValidationStatus::Valid:
        validation = tr("Valid");
        break;
      case engine::InstanceRegistry::ValidationStatus::MissingRoot:
        validation = tr("MissingRoot");
        break;
      case engine::InstanceRegistry::ValidationStatus::MissingToml:
        validation = tr("MissingToml");
        break;
      case engine::InstanceRegistry::ValidationStatus::CorruptedToml:
        validation = tr("CorruptedToml");
        break;
      case engine::InstanceRegistry::ValidationStatus::UnregisteredGame:
        validation = tr("UnregisteredGame");
        break;
      }
    } else {
      validation = tr("(not in registry)");
    }
  } else {
    entry_type = tr("(no registry)");
    validation = tr("(no registry)");
  }
  add_kv_row(info_table_, tr("Entry type"), entry_type, false);
  add_kv_row(info_table_, tr("Created at"), created_at, true);
  add_kv_row(info_table_, tr("Last used"), last_used, true);
  add_kv_row(info_table_, tr("Is active"),
             active_name.isEmpty()
                 ? QStringLiteral("false")
                 : (QString::fromStdString(
                        instance_root_.filename().string()) == active_name
                        ? QStringLiteral("true")
                        : QStringLiteral("false")),
             false);
  add_kv_row(info_table_, tr("Validation"), validation, false);
  add_kv_row(info_table_, tr("Total instances"), QString::number(reg_total),
             false);

  // Group: Categories
  add_group_header(info_table_, tr("Categories"));
  auto &factory = engine::Category::Factory::instance();
  int factory_size = static_cast<int>(factory.categories().size());
  fs::path categories_path = instance_root_ / "categories.dat";
  bool cat_exists = fs::exists(categories_path);
  add_kv_row(info_table_, tr("Core set (knowledge)"),
             knowledge_ ? QString::fromStdString(knowledge_->get(
                              game_id_, "core_category_set", "(none)"))
                        : QStringLiteral("(no knowledge)"),
             false);
  add_kv_row(info_table_, tr("Factory size"), QString::number(factory_size),
             false);
  add_kv_row(info_table_, tr("categories.dat exists"), bool_text(cat_exists),
             false);
  add_kv_row(info_table_, tr("categories.dat path"),
             QString::fromStdString(categories_path.string()), true);
  // Nexus map size.
  fs::path nexus_map = instance_root_ / "nexuscatmap.dat";
  int nexus_size = 0;
  {
    std::ifstream f(nexus_map.string());
    if (f) {
      std::string line;
      while (std::getline(f, line))
        ++nexus_size;
    }
  }
  add_kv_row(info_table_, tr("Nexus cat map size"), QString::number(nexus_size),
             false);

  // Group: Profile
  add_group_header(info_table_, tr("Profile"));
  QString active_profile_name = tr("(none)");
  int profile_count = 0;
  int modlist_count = 0;
  int locked_count = 0;
  int plugins_count = 0;
  QString local_saves = QStringLiteral("-");
  QString local_settings = QStringLiteral("-");
  QString auto_arch_inv = QStringLiteral("-");
  if (active_profile_) {
    active_profile_name = QString::fromStdString(active_profile_->name());
    modlist_count = static_cast<int>(active_profile_->mods().size());
    local_saves = bool_text(active_profile_->local_saves());
    local_settings = bool_text(active_profile_->local_settings());
    auto_arch_inv =
        bool_text(active_profile_->automatic_archive_invalidation());
    try {
      locked_count =
          static_cast<int>(active_profile_->read_locked_order().size());
      plugins_count = static_cast<int>(active_profile_->read_plugins().size());
    } catch (...) {
    }
    // Count profile directories under profiles_dir.
    fs::path profiles_dir = instance_root_ / "profiles";
    std::error_code ec;
    if (fs::is_directory(profiles_dir, ec)) {
      for (auto &e : fs::directory_iterator(profiles_dir, ec)) {
        if (e.is_directory(ec))
          ++profile_count;
      }
    }
  } else if (!instance_root_.empty()) {
    fs::path profiles_dir = instance_root_ / "profiles";
    std::error_code ec;
    if (fs::is_directory(profiles_dir, ec)) {
      for (auto &e : fs::directory_iterator(profiles_dir, ec)) {
        if (e.is_directory(ec)) {
          if (profile_count == 0)
            active_profile_name =
                QString::fromStdString(e.path().filename().string());
          ++profile_count;
        }
      }
    }
  }
  add_kv_row(info_table_, tr("Active profile"), active_profile_name, true);
  add_kv_row(info_table_, tr("Profiles (count)"),
             QString::number(profile_count), false);
  add_kv_row(info_table_, tr("Modlist entries"), QString::number(modlist_count),
             false);
  add_kv_row(info_table_, tr("Local saves"), local_saves, false);
  add_kv_row(info_table_, tr("Local settings"), local_settings, false);
  add_kv_row(info_table_, tr("Auto archive invalidation"), auto_arch_inv,
             false);
  add_kv_row(info_table_, tr("Locked plugins"), QString::number(locked_count),
             false);
  add_kv_row(info_table_, tr("plugins.txt entries"),
             QString::number(plugins_count), false);

  // Group: Game Knowledge
  add_group_header(info_table_, tr("Game knowledge"));
  if (knowledge_) {
    int reg_games = static_cast<int>(knowledge_->registered_games().size());
    add_kv_row(info_table_, tr("Registered games"), QString::number(reg_games),
               false);
    auto keys = knowledge_->keys_for(game_id_);
    QString keys_str =
        keys.empty() ? QStringLiteral("(none)") : QString::fromStdString([&]() {
          std::string joined;
          for (const auto &k : keys) {
            if (!joined.empty())
              joined += ", ";
            joined += k;
          }
          return joined;
        }());
    add_kv_row(info_table_, tr("Keys (this game)"), keys_str, true);
    add_kv_row(
        info_table_, tr("disable_mechanism"),
        QString::fromStdString(knowledge_->get(
            game_id_, "disable_mechanism", engine::kDefaultDisableMechanism)),
        false);
    add_kv_row(info_table_, tr("delayed_disable"),
               bool_text(engine::delayed_disable_for(*knowledge_, game_id_)),
               false);
    add_kv_row(info_table_, tr("creation_club_file"),
               QString::fromStdString(
                   engine::creation_club_file_for(*knowledge_, game_id_)),
               true);
    add_kv_row(info_table_, tr("deploy_strategy (knowledge)"),
               QString::fromStdString(
                   engine::deploy_strategy_for(*knowledge_, game_id_)),
               false);
    add_kv_row(
        info_table_, tr("deploy_prefix"),
        QString::fromStdString(knowledge_->get(game_id_, "deploy_prefix", "")),
        false);
    add_kv_row(
        info_table_, tr("mods_subpath"),
        QString::fromStdString(knowledge_->get(game_id_, "mods_subpath", "")),
        false);
    add_kv_row(info_table_, tr("game_mods_dir (plugin hook)"),
               QString::fromStdString(
                   engine::plugin_game_mods_dir(*knowledge_, game_id_)),
               true);
  } else {
    add_kv_row(info_table_, tr("(knowledge unavailable)"), QString(), false);
  }

  // Group: App
  add_group_header(info_table_, tr("App"));
  add_kv_row(info_table_, tr("GMM version"), QStringLiteral(VERSION), true);
  add_kv_row(info_table_, tr("Qt version"), QString::fromUtf8(qVersion()),
             true);
  add_kv_row(info_table_, tr("Kernel / OS"),
             QStringLiteral("%1 (%2)").arg(QSysInfo::kernelVersion(),
                                           QSysInfo::prettyProductName()),
             true);
  add_kv_row(info_table_, tr("CPU model"),
             QString::fromStdString(cpu_model_name()), true);
  add_kv_row(info_table_, tr("CPU cores (online)"),
             QString::number(online_cpu_count()), false);
  add_kv_row(info_table_, tr("Default instances dir"),
             QString::fromStdString(engine::default_instances_dir().string()),
             true);
}

// ---------------------------------------------------------------------------
// Stats refresh - labels (and pushes the simple single-sample charts that
// reuse the same source: CPU%, PSS MiB, VmData MiB, disk KiB/s, network
// KiB/s, jitter ms). The disk/net/jitter charts only sample at the chart
// timer (1 Hz) so the deltas are per-second; the labels run on whatever
// interval the user picked, and we still push to the charts so the chart
// stays continuous regardless of label cadence.
// ---------------------------------------------------------------------------

void DebugWindow::refresh_populated() {
  // Both tabs rebuild from the cached state. Cheap (the registry/path
  // readers are bounded) and called only on instance/registry/profile
  // changes, not on every timer tick.
  populate_paths();
  populate_info();
  // Network log is appended every refresh_stats tick; the populate here
  // keeps it fresh on instance switches / registry rebuilds too.
  populate_network();
}

void DebugWindow::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  // Safety net for "instance switched while the debug window was hidden":
  // SettingsController::set_game_info() pushes the new state via
  // rebind_for_instance() which already repopulates. But if a caller
  // forgot (or rebuilt the DebugWindow), the cached tables would be stale
  // until the next refresh tick. Re-running the populators on every show
  // is cheap and harmless when nothing changed (QTableWidget::setRowCount
  // is a no-op for empty rebuilds).
  refresh_populated();
  // Restart the periodic timers so a previously hidden dialog resumes
  // refreshing. hideEvent() pauses them.
  if (refresh_timer_) refresh_timer_->start(refresh_interval_ * 1000);
  if (chart_timer_) chart_timer_->start(1000);
}

void DebugWindow::hideEvent(QHideEvent *event) {
  // Pause the periodic refreshes while hidden so we don't burn CPU on a
  // dialog nobody is looking at. populate_network() in particular would
  // otherwise rebuild 500 rows x 6 cells every refresh tick (1-2s),
  // which manifested as a UI giga-freeze in the nfpb review.
  if (refresh_timer_) refresh_timer_->stop();
  if (chart_timer_) chart_timer_->stop();
  QDialog::hideEvent(event);
}

void DebugWindow::refresh_charts() {
  if (!cpu_chart_)
    return;
  // Re-read everything for chart sampling; refresh_stats does the label
  // updates, this path is purely for chart series.
  auto proc_stat = read_proc("/proc/self/stat");
  auto sys_stat = read_proc("/proc/stat");
  auto status_str = read_proc("/proc/self/status");
  auto io_str = read_proc("/proc/self/io");

  double cpu_pct = 0.0;
  unsigned long pss_kb = 0;
  unsigned long vmdata_kb = 0;
  double disk_read_kbs = 0.0;
  double disk_write_kbs = 0.0;
  double rx_kbs = 0.0;
  double tx_kbs = 0.0;
  double jitter_ms = 0.0;

  // --- CPU% ---
  if (!proc_stat.empty() && !sys_stat.empty()) {
    unsigned long proc_utime = parse_after(proc_stat, 13);
    unsigned long proc_stime = parse_after(proc_stat, 14);
    unsigned long proc_ticks = proc_utime + proc_stime;

    unsigned long su, sn, ss, sid, siow, sir, sq, sst;
    // sscanf returns 8 on success, < 8 on format mismatch. We don't need
    // to bail: missing columns just zero out the system total.
    int got =
        std::sscanf(sys_stat.c_str(), "cpu  %lu %lu %lu %lu %lu %lu %lu %lu",
                    &su, &sn, &ss, &sid, &siow, &sir, &sq, &sst);
    if (got == 8) {
      unsigned long sys_total = su + sn + ss + sid + siow + sir + sq + sst;
      if (!first_cpu_chart_) {
        unsigned long dp = proc_ticks - prev_proc_ticks_chart_;
        unsigned long ds = sys_total - prev_sys_total_chart_;
        if (ds > 0)
          cpu_pct = 100.0 * static_cast<double>(dp) / static_cast<double>(ds);
      }
      first_cpu_chart_ = false;
      prev_proc_ticks_chart_ = proc_ticks;
      prev_sys_total_chart_ = sys_total;
    }
  }

  // --- RAM (Pss) + Heap (VmData) ---
  pss_kb = parse_kb_line(status_str, "VmRSS:");
  vmdata_kb = parse_kb_line(status_str, "VmData:");

  // --- Disk IO (KiB/s). We sample at 1 Hz so divide by 1024 (KiB), not by
  //     the label interval. ---
  if (!io_str.empty()) {
    unsigned long long r = parse_io_value(io_str, "read_bytes:");
    unsigned long long w = parse_io_value(io_str, "write_bytes:");
    if (!first_io_chart_) {
      unsigned long long dr = r - prev_read_bytes_chart_;
      unsigned long long dw = w - prev_write_bytes_chart_;
      disk_read_kbs = static_cast<double>(dr) / 1024.0;
      disk_write_kbs = static_cast<double>(dw) / 1024.0;
    }
    first_io_chart_ = false;
    prev_read_bytes_chart_ = r;
    prev_write_bytes_chart_ = w;
  }

  // --- Network IO ---
  auto net = sum_net_counters();
  if (!first_net_chart_) {
    double dr = static_cast<double>(net.rx - prev_rx_bytes_chart_);
    double dw = static_cast<double>(net.tx - prev_tx_bytes_chart_);
    rx_kbs = dr / 1024.0;
    tx_kbs = dw / 1024.0;
  }
  first_net_chart_ = false;
  prev_rx_bytes_chart_ = net.rx;
  prev_tx_bytes_chart_ = net.tx;

  // --- Event-loop jitter (ms deviation from 1000 ms target). ---
  if (!first_jitter_) {
    qint64 ns = jitter_timer_.nsecsElapsed();
    jitter_timer_.restart();
    double actual_ms = static_cast<double>(ns) / 1e6;
    jitter_ms = actual_ms - 1000.0;
    if (jitter_ms > 50.0)
      jitter_ms = 50.0;
    if (jitter_ms < -50.0)
      jitter_ms = -50.0;
  } else {
    first_jitter_ = false;
    jitter_timer_.restart();
  }

  // Push to charts.
  cpu_chart_->push_sample(cpu_pct);
  ram_chart_->push_sample(static_cast<double>(pss_kb) / 1024.0);
  heap_chart_->push_sample(static_cast<double>(vmdata_kb) / 1024.0);
  // Disk and Net use the two-series push so Read/Write (RX/TX) trend
  // independently instead of cancelling inside a single sum. The chart
  // widget's auto-scale honors clamp_negative so the Y lower bound
  // cannot drop below 0 even when both flows are idle.
  disk_chart_->push_samples(disk_read_kbs, disk_write_kbs);
  net_chart_->push_samples(rx_kbs, tx_kbs);
  jitter_chart_->push_sample(jitter_ms);

  // Headers.
  if (cpu_header_) {
    cpu_header_->setText(
        QStringLiteral("%1%  (%2 cores: %3)")
            .arg(cpu_pct, 0, 'f', 1)
            .arg(online_cpu_count())
            .arg(QString::fromStdString(cpu_model_name()).left(60)));
  }
  if (ram_header_) {
    ram_header_->setText(QStringLiteral("RSS %1 MiB  VmHWM %2 MiB")
                             .arg(pss_kb / 1024)
                             .arg(parse_kb_line(status_str, "VmHWM:") / 1024));
  }
  if (heap_header_) {
    heap_header_->setText(
        QStringLiteral("Data %1 MiB  Peak %2 MiB")
            .arg(vmdata_kb / 1024)
            .arg(parse_kb_line(status_str, "VmPeak:") / 1024));
  }
  if (disk_header_) {
    disk_header_->setText(QStringLiteral("R %1 KiB/s  W %2 KiB/s")
                              .arg(QString::asprintf("%.1f", disk_read_kbs))
                              .arg(QString::asprintf("%.1f", disk_write_kbs)));
  }
  if (net_header_) {
    net_header_->setText(QStringLiteral("RX %1 KiB/s  TX %2 KiB/s")
                             .arg(QString::asprintf("%.1f", rx_kbs))
                             .arg(QString::asprintf("%.1f", tx_kbs)));
  }
  if (jitter_header_) {
    jitter_header_->setText(
        QStringLiteral("%1 ms").arg(QString::asprintf("%+.1f", jitter_ms)));
  }
}

void DebugWindow::refresh_stats() {
  // Read everything once.
  auto proc_stat = read_proc("/proc/self/stat");
  auto sys_stat = read_proc("/proc/stat");
  auto rollup = read_proc("/proc/self/smaps_rollup");
  auto status_str = read_proc("/proc/self/status");
  auto io_str = read_proc("/proc/self/io");
  auto uptime_str = read_proc("/proc/uptime");

  // --- CPU ---
  if (!proc_stat.empty() && !sys_stat.empty()) {
    unsigned long proc_utime = parse_after(proc_stat, 13);
    unsigned long proc_stime = parse_after(proc_stat, 14);
    unsigned long proc_ticks = proc_utime + proc_stime;

    unsigned long su, sn, ss, sid, siow, sir, sq, sst;
    int got =
        std::sscanf(sys_stat.c_str(), "cpu  %lu %lu %lu %lu %lu %lu %lu %lu",
                    &su, &sn, &ss, &sid, &siow, &sir, &sq, &sst);
    if (got == 8) {
      unsigned long sys_total = su + sn + ss + sid + siow + sir + sq + sst;
      if (!first_cpu_label_) {
        unsigned long dp = proc_ticks - prev_proc_ticks_label_;
        unsigned long ds = sys_total - prev_sys_total_label_;
        double pct =
            ds > 0 ? 100.0 * static_cast<double>(dp) / static_cast<double>(ds)
                   : 0.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
        if (cpu_label_)
          cpu_label_->setText(QString::fromUtf8(buf));
      }
      first_cpu_label_ = false;
      prev_proc_ticks_label_ = proc_ticks;
      prev_sys_total_label_ = sys_total;
    }
  }

  // --- RAM (Pss from smaps_rollup, VmRSS from status). The legacy label
  //     shows Pss; the chart reads VmRSS for a smoother curve. ---
  unsigned long pss = parse_kb_line(rollup, "Pss:");
  if (pss == 0)
    pss = parse_kb_line(status_str, "VmRSS:");
  if (ram_label_)
    ram_label_->setText(QStringLiteral("%1 MiB").arg(pss / 1024));

  // --- Disk IO (B/s or KiB/s) ---
  if (!io_str.empty()) {
    unsigned long long cur_read = parse_io_value(io_str, "read_bytes:");
    unsigned long long cur_write = parse_io_value(io_str, "write_bytes:");
    if (!first_io_label_) {
      auto delta_read = cur_read - prev_read_bytes_label_;
      auto delta_write = cur_write - prev_write_bytes_label_;
      // The label refresh interval may be >1s, so the deltas are
      // accumulated over refresh_interval_; divide by it for B/s.
      double interval_s = static_cast<double>(refresh_interval_);
      auto read_bs = delta_read / interval_s;
      auto write_bs = delta_write / interval_s;
      auto read_kbs = static_cast<unsigned long>(read_bs / 1024.0);
      auto write_kbs = static_cast<unsigned long>(write_bs / 1024.0);
      if (read_kbs > 0 || write_kbs > 0) {
        if (disk_label_)
          disk_label_->setText(QStringLiteral("R: %1 KiB/s  W: %2 KiB/s")
                                   .arg(read_kbs)
                                   .arg(write_kbs));
      } else {
        if (disk_label_)
          disk_label_->setText(QStringLiteral("R: %1 B/s  W: %2 B/s")
                                   .arg(static_cast<int>(read_bs))
                                   .arg(static_cast<int>(write_bs)));
      }
    }
    first_io_label_ = false;
    prev_read_bytes_label_ = cur_read;
    prev_write_bytes_label_ = cur_write;
  }

  // --- Process uptime ---
#ifdef __linux__
  if (!proc_stat.empty()) {
    auto start_ticks = parse_after(proc_stat, 21);
    long hz = ::sysconf(_SC_CLK_TCK);
    if (hz > 0 && start_ticks > 0 && !uptime_str.empty()) {
      auto space = uptime_str.find(' ');
      double sys_uptime = 0.0;
      try {
        sys_uptime = std::stod(uptime_str.substr(0, space));
      } catch (...) {
      }
      double proc_uptime = sys_uptime - static_cast<double>(start_ticks) / hz;
      if (proc_uptime < 0)
        proc_uptime = 0;
      int days = static_cast<int>(proc_uptime / 86400);
      int hours = static_cast<int>(std::fmod(proc_uptime, 86400) / 3600);
      int mins = static_cast<int>(std::fmod(proc_uptime, 3600) / 60);
      int secs = static_cast<int>(std::fmod(proc_uptime, 60));
      QString s;
      if (days > 0)
        s = QStringLiteral("%1d %2h %3m %4s")
                .arg(days)
                .arg(hours)
                .arg(mins)
                .arg(secs);
      else if (hours > 0)
        s = QStringLiteral("%1h %2m %3s").arg(hours).arg(mins).arg(secs);
      else if (mins > 0)
        s = QStringLiteral("%1m %2s").arg(mins).arg(secs);
      else
        s = QStringLiteral("%1s").arg(secs);
      if (uptime_label_)
        uptime_label_->setText(s);
    }
  }
#else
  // Non-Linux: procfs not available. Show a placeholder so the row still
  // looks like a stat rather than an empty cell. The charts/headers in
  // refresh_charts() handle their own non-Linux fallbacks.
  if (uptime_label_)
    uptime_label_->setText(QStringLiteral("(unsupported)"));
#endif
  // Refresh the Network log every tick. The ring buffer is bounded so the
  // cost is negligible and the user sees new requests without having to
  // close/reopen the dialog.
  populate_network();
}

// ---------------------------------------------------------------------------
// Network tab - the request log surfaced by engine::network::
// ---------------------------------------------------------------------------

void DebugWindow::setup_network_tab() {
  auto *tab = new QWidget;
  auto *v = new QVBoxLayout(tab);
  v->setContentsMargins(4, 4, 4, 4);

  network_table_ = new QTableWidget(0, 6, tab);
  network_table_->setHorizontalHeaderLabels(
      {tr("Caller"), tr("Method"), tr("URL (redacted)"), tr("Status"),
       tr("Time (ms)"), tr("Error")});
  network_table_->verticalHeader()->hide();
  network_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  network_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  network_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  network_table_->setAlternatingRowColors(true);
  network_table_->setShowGrid(false);
  network_table_->setWordWrap(false);
  auto *hh = network_table_->horizontalHeader();
  hh->setStretchLastSection(true);
  hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  hh->setSectionResizeMode(2, QHeaderView::Stretch);
  hh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  hh->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  hh->setSectionResizeMode(5, QHeaderView::ResizeToContents);

  // Copy / context menu - same pattern as Paths / Info tabs.
  network_table_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(network_table_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint &pos) {
            auto *item = network_table_->itemAt(pos);
            if (!item)
              return;
            QStringList row;
            for (int c = 0; c < network_table_->columnCount(); ++c) {
              auto *cell = network_table_->item(item->row(), c);
              row << (cell ? cell->text() : QString());
            }
            QGuiApplication::clipboard()->setText(row.join(QStringLiteral(" | ")));
          });

  v->addWidget(network_table_);
  network_tab_index_ = tabs_->addTab(tab, tr("Network"));

  populate_network();
}

void DebugWindow::populate_network() {
  if (!network_table_)
    return;

  // nfpb perf fix: skip the table rebuild entirely when the Network tab
  // is not currently visible. refresh_stats() still fires on the
  // label-timer (1-2s by default), but doing the rebuild burns ~3000
  // QTableWidgetItem allocations + mutex contention for a tab nobody is
  // looking at. hideEvent() also pauses the timer; this is the belt to
  // those braces.
  if (network_tab_index_ < 0 || tabs_ == nullptr ||
      tabs_->currentIndex() != network_tab_index_) {
    return;
  }

  // Pull a bounded snapshot (100 rows, not 500) so the table stays light
  // even on busy networks. The ring buffer still holds 2000 entries
  // internally; users wanting the full history can scroll/filter later.
  constexpr std::size_t kMaxRows = 100;
  auto entries = engine::network::instance().log_snapshot(kMaxRows);
  if (entries.empty()) {
    if (network_table_->rowCount() != 0) {
      network_table_->setRowCount(0);
      last_network_log_id_ = 0;
    }
    return;
  }

  // Diff-based skip: if the newest entry id hasn't advanced since the
  // last rebuild, do nothing. The id is monotonic so equality implies
  // nothing new arrived. Empty log -> id 0, also fine.
  const std::uint64_t newest_id = entries.front().id;
  if (newest_id == last_network_log_id_ &&
      network_table_->rowCount() == static_cast<int>(entries.size())) {
    return;
  }

  network_table_->setRowCount(static_cast<int>(entries.size()));
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto &e = entries[i];
    int row = static_cast<int>(i);
    auto set_cell = [&](int col, const QString &s) {
      auto *it = new QTableWidgetItem(s);
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
      network_table_->setItem(row, col, it);
    };
    set_cell(0, QString::fromStdString(e.caller));
    set_cell(1, QString::fromStdString(e.method));
    set_cell(2, QString::fromStdString(e.url_redacted));
    if (e.http_code > 0) {
      set_cell(3, QString::number(e.http_code));
    } else {
      set_cell(3, QStringLiteral("-"));
    }
    set_cell(4, QString::number(e.total_time_ms, 'f', 1));
    set_cell(5, QString::fromStdString(e.curl_error));
  }
  // Scroll to the top - newest first.
  if (network_table_->rowCount() > 0)
    network_table_->scrollToTop();
  last_network_log_id_ = newest_id;
}

// ---------------------------------------------------------------------------
// procfs helpers
// ---------------------------------------------------------------------------

std::string DebugWindow::read_proc(const char *path) {
  std::ifstream f(path);
  if (!f)
    return {};
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

unsigned long DebugWindow::parse_after(const std::string &s, int field_index) {
  // /proc/self/stat field 2 is "(comm)" which can contain spaces.
  // Skip it by finding the last ')'.
  auto end_comm = s.rfind(')');
  if (end_comm == std::string::npos)
    return 0;
  auto pos = s.find(' ', end_comm); // field 3 (state) starts here
  int idx = 2;                      // we've looked past fields 1 and 2
  while (pos != std::string::npos && idx < field_index) {
    ++idx;
    auto next = s.find(' ', pos + 1);
    if (next == std::string::npos)
      break;
    pos = next;
  }
  if (idx != field_index)
    return 0;
  auto start = s.find_first_not_of(' ', pos + 1);
  if (start == std::string::npos)
    return 0;
  auto end = s.find(' ', start);
  auto val = s.substr(start, end - start);
  unsigned long result = 0;
  std::from_chars(val.data(), val.data() + val.size(), result);
  return result;
}

} // namespace ui

// AUTOMOC (set in CMakeLists.txt) generates and includes the moc output
// automatically; an explicit #include "moc_debug_window.cpp" would create
// a duplicate symbol and rely on the AUTOMOC include-path search. Every
// other Q_OBJECT widget in this codebase relies on AUTOMOC alone.
