#pragma once

#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QWidget>
#include <QStringList>
#include <QTimer>

class QFrame;
class QToolButton;

namespace ui {

class StatusBar : public QWidget {
    Q_OBJECT
public:
    explicit StatusBar(QWidget* parent = nullptr);

    void set_status(const QString& text);

    // Configure what the status bar shows for the current game
    void set_sources(const QStringList& sources);  // e.g. {"Nexus Mods", "Steam"}

signals:
    void pipeline_clicked();

private:
    void refresh_pipeline_indicator();
    void refresh_nexus_source();

    QHBoxLayout* layout_ = nullptr;
    QLabel* status_label_ = nullptr;
    QList<QLabel*> source_labels_;
    QMap<QString, QLabel*> source_labels_by_name_;
    QFrame* separator_ = nullptr;
    QToolButton* pipeline_button_ = nullptr;
    QTimer* pipeline_timer_ = nullptr;
};

}  // namespace ui
