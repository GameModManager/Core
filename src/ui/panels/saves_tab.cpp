#include "ui/panels/saves_tab.h"
#include "ui/panels/panel_utils.h"

#include "engine/core/log/logger.h"

#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace ui {

// --- SavesTab ---
QString SavesTab::missing_tooltip(const SavesScanResultEntry& entry) {
    QStringList lines;
    for (const auto& asset : entry.missing) {
        QString line = QString::fromStdString(asset.plugin_name);
        if (asset.inactive) {
            line += tr(" (disabled)");
        } else if (!asset.providing_mods.empty()) {
            QStringList providers;
            for (const auto& m : asset.providing_mods)
                providers << QString::fromStdString(m);
            line += " - " + providers.join(", ");
        }
        lines << line;
    }
    return lines.isEmpty() ? tr("No missing plugins") : lines.join('\n');
}

SavesTab::SavesTab(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    table_ = make_table(3, {tr("Name"), tr("File"), tr("Missing")}, this);
    table_->setObjectName("savesTable");
    table_->setMouseTracking(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table_, 1);

    // No directory watcher: scans run once at game load and after a delete,
    // never in the background (a watched Proton-prefix Saves dir churns and
    // spammed 1-per-second re-scans).
    scan_thread_ = new SavesScanThread(this);
    connect(scan_thread_->worker(), &SavesScanWorker::finished,
            this, &SavesTab::on_scan_finished, Qt::QueuedConnection);

    connect(table_, &QTableWidget::itemEntered,
            this, &SavesTab::on_item_entered);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &SavesTab::on_selection_changed);
    connect(table_, &QTableWidget::customContextMenuRequested,
            this, &SavesTab::on_context_menu);

    table_->viewport()->installEventFilter(this);
    table_->installEventFilter(this);
}

SavesTab::~SavesTab() {
    delete info_popup_;
}

void SavesTab::set_saves_dir(const std::filesystem::path& dir) {
    saves_dir_ = dir;
}

void SavesTab::set_saves(SavesScanResult result) {
    scanning_ = false;
    saves_ = std::move(result);
    table_->setRowCount(0);
    table_->setRowCount(static_cast<int>(saves_.entries.size()));
    engine::Logger::instance().debug(
        "Saves scan landed: " + std::to_string(saves_.entries.size()) +
        " save(s) from " + saves_.saves_dir.string());
    for (int row = 0; row < saves_.entries.size(); ++row) {
        const auto& entry = saves_.entries[row];
        auto* name = new QTableWidgetItem(
            QString::fromStdString(entry.save.display_name()));
        auto* file = new QTableWidgetItem(
            QString::fromStdString(entry.save.file_path.filename().string()));
        file->setToolTip(QString::fromStdString(entry.save.file_path.string()));
        int missing = static_cast<int>(entry.missing.size());
        auto* miss = new QTableWidgetItem(missing > 0 ? QString::number(missing)
                                                      : QString());
        miss->setToolTip(missing_tooltip(entry));
        table_->setItem(row, kColumnName, name);
        table_->setItem(row, kColumnFile, file);
        table_->setItem(row, kColumnMissing, miss);
    }
    hide_save_info();
}

void SavesTab::clear_saves() {
    scanning_ = false;
    saves_ = {};
    table_->setRowCount(0);
    hide_save_info();
}

const engine::SaveGame* SavesTab::save_at(int row) const {
    if (row < 0 || row >= saves_.entries.size()) return nullptr;
    return &saves_.entries[row].save;
}

const std::vector<engine::SaveMissingAsset>* SavesTab::missing_at(int row) const {
    if (row < 0 || row >= saves_.entries.size()) return nullptr;
    return &saves_.entries[row].missing;
}

void SavesTab::request_scan(SavesScanRequest request) {
    scanning_ = true;
    scan_thread_->start(std::move(request));
}

void SavesTab::on_scan_finished(SavesScanResult result) {
    set_saves(std::move(result));
}

void SavesTab::on_item_entered(QTableWidgetItem* item) {
    if (!item) return;
    show_save_info(item->row());
}

void SavesTab::on_selection_changed() {
    const auto selected = table_->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        hide_save_info();
        return;
    }
    show_save_info(selected.first().row());
}

void SavesTab::show_save_info(int row) {
    const auto* save = save_at(row);
    if (!save) return;
    const auto* missing = missing_at(row);

    hide_save_info();
    auto* popup = new QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    info_popup_ = popup;

    auto* v = new QVBoxLayout(popup);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(2);

    // Screenshot (MO2 GamebryoSaveGameInfoWidget::screenshotLabel).
    if (save->screenshot_width > 0 && save->screenshot_height > 0 &&
        save->screenshot.size() >=
            static_cast<std::size_t>(save->screenshot_width) *
                save->screenshot_height * 3) {
        const bool rgba = save->screenshot.size() ==
                          static_cast<std::size_t>(save->screenshot_width) *
                              save->screenshot_height * 4;
        QImage img(static_cast<const uchar*>(save->screenshot.data()),
                   save->screenshot_width, save->screenshot_height,
                   rgba ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
        auto* shot = new QLabel(popup);
        shot->setPixmap(QPixmap::fromImage(img));
        v->addWidget(shot);
    }

    const auto add_row = [&](const QString& label, const QString& value) {
        auto* row = new QLabel(QString("<b>%1</b> %2").arg(label, value), popup);
        v->addWidget(row);
    };
    add_row(tr("Character:"), QString::fromStdString(save->pc_name));
    add_row(tr("Level:"), QString::number(save->pc_level));
    add_row(tr("Location:"), QString::fromStdString(save->pc_location));
    add_row(tr("Save #:"), QString::number(save->save_number));
    add_row(tr("Time:"), QLocale().toString(
                              QDateTime::fromSecsSinceEpoch(save->creation_time),
                              QLocale::ShortFormat));

    if (save->has_script_extender_file()) {
        auto* skse = new QLabel(tr("<b>Has Script Extender Data</b>"), popup);
        v->addWidget(skse);
    }

    auto* header = new QLabel(tr("<i>Missing ESPs</i>"), popup);
    v->addWidget(header);
    int shown = 0;
    if (missing && !missing->empty()) {
        for (const auto& asset : *missing) {
            if (shown >= 7) break;
            ++shown;
            auto* label = new QLabel(QString("    %1")
                                         .arg(QString::fromStdString(asset.plugin_name)),
                                     popup);
            v->addWidget(label);
        }
        if (static_cast<std::size_t>(shown) < missing->size()) {
            v->addWidget(new QLabel("...", popup));
        }
    } else {
        v->addWidget(new QLabel(tr("    None"), popup));
    }

    popup->adjustSize();
    popup->setWindowOpacity(
        popup->style()->styleHint(QStyle::SH_ToolTipLabel_Opacity, nullptr, popup) /
        qreal(255.0));

    // Position near the cursor, flipped when it would leave the screen
    // (MO2 displaySaveGameInfo geometry logic).
    QPoint pos = QCursor::pos();
    const QRect screen = QGuiApplication::screenAt(pos)
                             ? QGuiApplication::screenAt(pos)->geometry()
                             : QGuiApplication::primaryScreen()->geometry();
    if (pos.x() + popup->width() > screen.right()) {
        pos.setX(pos.x() - popup->width() - 2);
    } else {
        pos.setX(pos.x() + 5);
    }
    if (pos.y() + popup->height() > screen.bottom()) {
        pos.setY(pos.y() - popup->height() - 10);
    } else {
        pos.setY(pos.y() + 20);
    }
    popup->move(pos);
    popup->show();
}

void SavesTab::hide_save_info() {
    if (info_popup_) {
        info_popup_->close();
        info_popup_ = nullptr;
    }
}

bool SavesTab::eventFilter(QObject* object, QEvent* event) {
    if (object == table_ || object == table_->viewport()) {
        if (event->type() == QEvent::Leave || event->type() == QEvent::WindowDeactivate) {
            hide_save_info();
        } else if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Delete) {
                on_delete_key();
                return true;
            }
        }
    }
    return QWidget::eventFilter(object, event);
}

void SavesTab::on_delete_key() {
    const auto selected = table_->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    QStringList delete_files;
    QStringList label_rows;
    for (const auto& idx : selected) {
        const auto* save = save_at(idx.row());
        if (!save) continue;
        delete_files << QString::fromStdString(save->file_path.string());
        label_rows << save->file_path.filename().string().c_str();
        // The script-extender co-save travels with its save (MO2 allFiles()).
        const auto skse = save->file_path;
        std::filesystem::path co = skse;
        co.replace_extension("skse");
        if (std::filesystem::is_regular_file(co)) {
            delete_files << QString::fromStdString(co.string());
        }
    }
    if (delete_files.isEmpty()) return;

    QString msg;
    const int total = label_rows.size();
    for (int i = 0; i < std::min(10, total); ++i) {
        msg += "<li>" + label_rows[i].toHtmlEscaped() + "</li>";
    }
    if (total > 10) {
        msg += "<li><i>... " + tr("%1 more").arg(total - 10) + "</i></li>";
    }
    const auto answer = QMessageBox::question(
        this, tr("Confirm"),
        tr("Are you sure you want to remove the following %n save(s)?<br>"
           "<ul>%1</ul><br>Removed saves will be sent to the trash.",
           "", total).arg(msg),
        QMessageBox::Yes | QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        emit delete_requested(delete_files);
    }
}

void SavesTab::on_context_menu(const QPoint& pos) {
    const auto selected = table_->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;

    QMenu menu(this);
    menu.addAction(tr("Delete %n save(s)", "", selected.size()),
                   this, &SavesTab::on_delete_key);
    menu.addAction(tr("Open in file manager"), this, [this] {
        const auto sel = table_->selectionModel()->selectedRows();
        if (sel.isEmpty()) return;
        const auto* save = save_at(sel.first().row());
        if (!save) return;
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QString::fromStdString(save->file_path.parent_path().string())));
    });
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

}  // namespace ui
