#include "ui/widgets/instance_statistics_dialog.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QProcess>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <fstream>
#include <vector>

namespace ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint64_t InstanceStatisticsDialog::dir_size(const std::filesystem::path& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

std::string InstanceStatisticsDialog::format_size(uint64_t bytes) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }
    char buf[64];
    if (unit == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s",
                      static_cast<unsigned long long>(bytes), units[unit]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Known size-explorer executables (Linux)
// ---------------------------------------------------------------------------

static const std::vector<std::pair<const char*, const char*>> kExplorers = {
    {"filelight",   "Filelight from KDE"},
    {"qdirstat",   "QDirStat"},
    {"baobab",     "Baobab (GNOME Disk Usage Analyzer)"},
    {"ncdu",       "NCurses Disk Usage"},
};

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

InstanceStatisticsDialog::InstanceStatisticsDialog(
    const std::filesystem::path& instance_root,
    const std::filesystem::path& cache_dir,
    int total_mods,
    QWidget* parent)
    : QDialog(parent)
    , instance_root_(instance_root) {

    setWindowTitle(tr("Instance Statistics"));
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // Compute sizes
    auto mods_dir = instance_root / "mods";
    auto cache_dir_path = cache_dir;
    auto downloads_dir = instance_root / "downloads";
    auto logs_dir = instance_root / "logs";

    uint64_t instance_bytes = dir_size(instance_root);
    uint64_t mods_bytes = dir_size(mods_dir);
    uint64_t cache_bytes = dir_size(cache_dir_path);
    uint64_t downloads_bytes = dir_size(downloads_dir);
    uint64_t logs_bytes = dir_size(logs_dir);

    auto fmt = [this](const char* label, uint64_t bytes, QLabel** out) {
        auto* row = new QHBoxLayout;
        auto* lbl = new QLabel(tr(label));
        lbl->setObjectName("statLabel");
        *out = new QLabel(QString::fromStdString(format_size(bytes)));
        (*out)->setAlignment(Qt::AlignRight);
        row->addWidget(lbl);
        row->addWidget(*out, 1);
        return row;
    };

    layout->addLayout(fmt("Instance size:",   instance_bytes,   &instance_size_label_));
    layout->addLayout(fmt("Mods size:",       mods_bytes,       &mods_size_label_));
    layout->addLayout(fmt("Cache size:",      cache_bytes,      &cache_size_label_));
    layout->addLayout(fmt("Downloads size:",  downloads_bytes,  &downloads_size_label_));
    layout->addLayout(fmt("Logs size:",       logs_bytes,       &logs_size_label_));

    auto* mods_row = new QHBoxLayout;
    auto* mods_lbl = new QLabel(tr("Total mods:"));
    mods_lbl->setObjectName("statLabel");
    total_mods_label_ = new QLabel(QString::number(total_mods));
    total_mods_label_->setAlignment(Qt::AlignRight);
    mods_row->addWidget(mods_lbl);
    mods_row->addWidget(total_mods_label_, 1);
    layout->addLayout(mods_row);

    // Write statistics.ini to cache
    auto stats_path = cache_dir / "statistics.ini";
    {
        std::ofstream f(stats_path);
        if (f) {
            f << "[Statistics]\n"
              << "instance_bytes = " << instance_bytes << "\n"
              << "mods_bytes = " << mods_bytes << "\n"
              << "cache_bytes = " << cache_bytes << "\n"
              << "downloads_bytes = " << downloads_bytes << "\n"
              << "logs_bytes = " << logs_bytes << "\n"
              << "total_mods = " << total_mods << "\n";
        }
    }

    // --- Size explorer button ---
    layout->addSpacing(12);

    explorer_btn_ = new QPushButton(tr("Open in size explorer..."));
    std::string explorer_path;
    std::string explorer_name;
    for (const auto& [exe, display] : kExplorers) {
        auto found = QStandardPaths::findExecutable(
            QString::fromUtf8(exe)).toStdString();
        if (!found.empty()) {
            explorer_path = found;
            explorer_name = display;
            break;
        }
    }

    if (explorer_path.empty()) {
        explorer_btn_->setEnabled(false);
        explorer_btn_->setToolTip(
            tr("Install Filelight from KDE for a visual disk usage view"));
    } else {
        explorer_btn_->setToolTip(QString::fromStdString(explorer_name));
        connect(explorer_btn_, &QPushButton::clicked, this,
                [this, path = std::move(explorer_path)]() {
            QProcess::startDetached(
                QString::fromStdString(path),
                {QString::fromStdString(instance_root_.string())});
        });
    }

    layout->addWidget(explorer_btn_);

    // Close button
    auto* close_btn = new QPushButton(tr("Close"));
    connect(close_btn, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(close_btn);
}

} // namespace ui
