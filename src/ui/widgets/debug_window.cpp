#include "ui/widgets/debug_window.h"
#include "engine/plugin_host/plugin_loader.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QPushButton>

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace ui {

DebugWindow::DebugWindow(const std::filesystem::path& instance_root,
                         const std::string& game_id,
                         const std::string& game_name,
                         engine::PluginLoader* plugin_loader,
                         std::function<void()> on_reload_ui,
                         QWidget* parent)
    : QDialog(parent, Qt::Window | Qt::WindowStaysOnTopHint)
    , instance_root_(instance_root)
    , game_id_(game_id)
    , game_name_(game_name)
    , plugin_loader_(plugin_loader) {

    setWindowTitle("Debug Panel");
    setMinimumSize(520, 420);
    setAttribute(Qt::WA_DeleteOnClose, false);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // --- Refresh interval controls ---
    auto* interval_row = new QHBoxLayout;
    interval_row->setAlignment(Qt::AlignLeft);
    auto* dec_btn = new QPushButton("-");
    dec_btn->setFixedWidth(28);
    interval_label_ = new QLabel(QString::number(refresh_interval_));
    interval_label_->setObjectName("debugValue");
    interval_label_->setAlignment(Qt::AlignCenter);
    interval_label_->setFixedWidth(30);
    auto* inc_btn = new QPushButton("+");
    inc_btn->setFixedWidth(28);
    auto* sec_label = new QLabel("s");
    sec_label->setObjectName("debugKey");
    interval_row->addWidget(dec_btn);
    interval_row->addWidget(interval_label_);
    interval_row->addWidget(inc_btn);
    interval_row->addWidget(sec_label);
    interval_row->addStretch();
    layout->addLayout(interval_row);

    connect(dec_btn, &QPushButton::clicked, this, [this]() {
        if (refresh_interval_ > 1) {
            refresh_interval_ -= 1;
            interval_label_->setText(QString::number(refresh_interval_));
            refresh_timer_->setInterval(refresh_interval_ * 1000);
        }
    });
    connect(inc_btn, &QPushButton::clicked, this, [this]() {
        if (refresh_interval_ < 60) {
            refresh_interval_ += 1;
            interval_label_->setText(QString::number(refresh_interval_));
            refresh_timer_->setInterval(refresh_interval_ * 1000);
        }
    });

    // --- Resource usage ---
    auto* res_group = new QGroupBox("Resource Usage");
    auto* res_layout = new QVBoxLayout(res_group);

    auto add_row = [&](const char* label, QLabel** out) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(label);
        lbl->setObjectName("debugKey");
        *out = new QLabel("-");
        (*out)->setObjectName("debugValue");
        (*out)->setAlignment(Qt::AlignRight);
        row->addWidget(lbl);
        row->addWidget(*out, 1);
        res_layout->addLayout(row);
    };

    add_row("CPU:", &cpu_label_);
    add_row("RAM:", &ram_label_);
    add_row("Disk IO:", &disk_label_);
    add_row("Uptime:", &uptime_label_);
    layout->addWidget(res_group);

    // --- Instance info ---
    auto* info_group = new QGroupBox("Instance");
    auto* info_layout = new QVBoxLayout(info_group);

    auto add_info = [&](const char* label, QLabel** out) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(label);
        lbl->setObjectName("debugKey");
        *out = new QLabel("-");
        (*out)->setAlignment(Qt::AlignRight);
        (*out)->setWordWrap(true);
        (*out)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(lbl);
        row->addWidget(*out, 1);
        info_layout->addLayout(row);
    };

    add_info("Game:", &game_label_);
    add_info("Instance root:", &instance_root_label_);
    add_info("Plugins:", &plugins_label_);
    layout->addWidget(info_group);

    layout->addStretch();

    // Button row
    auto* btn_row = new QHBoxLayout;
    btn_row->setAlignment(Qt::AlignCenter);
    btn_row->setSpacing(12);
    reload_ui_btn_ = new QPushButton("Reload UI");
    reload_ui_btn_->setToolTip("Re-read debug.qss and apply (Ctrl+Shift+R)");
    if (on_reload_ui) {
        connect(reload_ui_btn_, &QPushButton::clicked, this, [on_reload_ui]() {
            on_reload_ui();
        });
    }
    btn_row->addWidget(reload_ui_btn_);
    auto* close_btn = new QPushButton("Close");
    connect(close_btn, &QPushButton::clicked, this, [this]() { hide(); });
    btn_row->addWidget(close_btn);
    layout->addLayout(btn_row);

    // Initial values
    game_label_->setText(QString::fromStdString(game_name_ + " (" + game_id_ + ")"));
    instance_root_label_->setText(QString::fromStdString(instance_root_.string()));

    if (plugin_loader_) {
        std::string list;
        for (const auto& p : plugin_loader_->plugins()) {
            if (!list.empty()) list += "\n";
            list += "  " + p.game_display_name + " (" + p.game_id + ")";
        }
        if (list.empty()) list = "(none)";
        plugins_label_->setText(QString::fromStdString(list));
    }

    refresh_stats();

    refresh_timer_ = new QTimer(this);
    connect(refresh_timer_, &QTimer::timeout, this, &DebugWindow::refresh_stats);
    refresh_timer_->start(refresh_interval_ * 1000);
}

DebugWindow::~DebugWindow() {
    if (refresh_timer_) {
        refresh_timer_->stop();
    }
}

std::string DebugWindow::read_proc(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string DebugWindow::read_proc_line(const char* path, const char* prefix) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    auto plen = std::strlen(prefix);
    while (std::getline(f, line)) {
        if (line.compare(0, plen, prefix) == 0)
            return line;
    }
    return {};
}

static unsigned long parse_after(const std::string& s, int field_index) {
    // /proc/self/stat field 2 is "(comm)" which can contain spaces.
    // Skip it by finding the last ')'.
    auto end_comm = s.rfind(')');
    if (end_comm == std::string::npos) return 0;
    auto pos = s.find(' ', end_comm);  // field 3 (state) starts here
    int idx = 2;  // we've looked past fields 1 and 2
    while (pos != std::string::npos && idx < field_index) {
        ++idx;
        auto next = s.find(' ', pos + 1);
        if (next == std::string::npos) break;
        pos = next;
    }
    if (idx != field_index) return 0;
    auto start = s.find_first_not_of(' ', pos + 1);
    if (start == std::string::npos) return 0;
    auto end = s.find(' ', start);
    auto val = s.substr(start, end - start);
    unsigned long result;
    std::from_chars(val.data(), val.data() + val.size(), result);
    return result;
}

void DebugWindow::refresh_stats() {
    // --- Process CPU ---
    static unsigned long prev_proc_ticks = 0;
    static unsigned long prev_sys_total = 0;
    static bool first_cpu = true;

    auto proc_stat = read_proc("/proc/self/stat");
    auto sys_stat = read_proc("/proc/stat");

    if (!proc_stat.empty() && !sys_stat.empty()) {
        unsigned long proc_utime = parse_after(proc_stat, 13);
        unsigned long proc_stime = parse_after(proc_stat, 14);
        unsigned long proc_ticks = proc_utime + proc_stime;

        unsigned long sys_user, sys_nice, sys_sys, sys_idle, sys_iowait,
                      sys_irq, sys_softirq, sys_steal;
        std::sscanf(sys_stat.c_str(), "cpu  %lu %lu %lu %lu %lu %lu %lu %lu",
                    &sys_user, &sys_nice, &sys_sys, &sys_idle,
                    &sys_iowait, &sys_irq, &sys_softirq, &sys_steal);
        unsigned long sys_total = sys_user + sys_nice + sys_sys + sys_idle
                                 + sys_iowait + sys_irq + sys_softirq + sys_steal;

        if (!first_cpu) {
            unsigned long delta_proc = proc_ticks - prev_proc_ticks;
            unsigned long delta_sys = sys_total - prev_sys_total;
            double pct = delta_sys > 0
                ? 100.0 * static_cast<double>(delta_proc) / static_cast<double>(delta_sys)
                : 0.0;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
            cpu_label_->setText(buf);
        } else {
            first_cpu = false;
        }
        prev_proc_ticks = proc_ticks;
        prev_sys_total = sys_total;
    }

    // --- Process RAM (Pss from smaps_rollup - proportional set size) ---
    auto rollup = read_proc("/proc/self/smaps_rollup");
    auto parse_kb = [&](const char* key) -> unsigned long {
        auto pos = rollup.find(key);
        if (pos == std::string::npos) return 0;
        pos += std::strlen(key);
        while (pos < rollup.size() && (rollup[pos] == ' ' || rollup[pos] == '\t')) ++pos;
        auto end = rollup.find('\n', pos);
        auto val_str = rollup.substr(pos, end - pos);
        auto space = val_str.find(' ');
        if (space != std::string::npos) val_str = val_str.substr(0, space);
        return std::stoul(val_str);
    };
    auto pss_kb = parse_kb("Pss:");
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lu MiB", pss_kb / 1024);
        ram_label_->setText(buf);
    }

    // --- Process Disk IO ---
    static unsigned long long prev_read = 0, prev_write = 0;
    static bool first_io = true;

    auto io_str = read_proc("/proc/self/io");
    auto parse_io = [&](const char* key) -> unsigned long long {
        auto pos = io_str.find(key);
        if (pos == std::string::npos) return 0;
        pos += std::strlen(key);
        while (pos < io_str.size() && (io_str[pos] == ' ' || io_str[pos] == '\t')) ++pos;
        auto end = io_str.find('\n', pos);
        auto val_str = io_str.substr(pos, end - pos);
        unsigned long long result;
        std::from_chars(val_str.data(), val_str.data() + val_str.size(), result);
        return result;
    };

    if (!io_str.empty()) {
        unsigned long long cur_read = parse_io("read_bytes:");
        unsigned long long cur_write = parse_io("write_bytes:");

        if (!first_io) {
            auto delta_read = cur_read - prev_read;
            auto delta_write = cur_write - prev_write;
            auto read_kbs = delta_read / 2048;
            auto write_kbs = delta_write / 2048;
            char buf[64];
            if (read_kbs > 0 || write_kbs > 0) {
                std::snprintf(buf, sizeof(buf), "R: %llu KiB/s  W: %llu KiB/s",
                              read_kbs, write_kbs);
            } else {
                std::snprintf(buf, sizeof(buf), "R: %llu B/s  W: %llu B/s",
                              delta_read / 2, delta_write / 2);
            }
            disk_label_->setText(buf);
        } else {
            first_io = false;
        }
        prev_read = cur_read;
        prev_write = cur_write;
    }

    // --- Process uptime ---
    if (!proc_stat.empty()) {
        auto start_ticks = parse_after(proc_stat, 21);
        long hz = sysconf(_SC_CLK_TCK);
        if (hz > 0 && start_ticks > 0) {
            auto uptime_str = read_proc("/proc/uptime");
            if (!uptime_str.empty()) {
                auto space = uptime_str.find(' ');
                if (space != std::string::npos) uptime_str = uptime_str.substr(0, space);
                double sys_uptime = std::stod(uptime_str);
                double proc_uptime = sys_uptime - static_cast<double>(start_ticks) / hz;
                if (proc_uptime < 0) proc_uptime = 0;
                int days = static_cast<int>(proc_uptime / 86400);
                int hours = static_cast<int>(std::fmod(proc_uptime, 86400) / 3600);
                int mins = static_cast<int>(std::fmod(proc_uptime, 3600) / 60);
                int secs = static_cast<int>(std::fmod(proc_uptime, 60));
                char buf[48];
                if (days > 0)
                    std::snprintf(buf, sizeof(buf), "%dd %dh %dm %ds", days, hours, mins, secs);
                else if (hours > 0)
                    std::snprintf(buf, sizeof(buf), "%dh %dm %ds", hours, mins, secs);
                else if (mins > 0)
                    std::snprintf(buf, sizeof(buf), "%dm %ds", mins, secs);
                else
                    std::snprintf(buf, sizeof(buf), "%ds", secs);
                uptime_label_->setText(buf);
            }
        }
    }
}

} // namespace ui
