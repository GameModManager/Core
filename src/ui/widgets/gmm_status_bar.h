#pragma once

#include <QHBoxLayout>
#include <QLabel>
#include <QWidget>

namespace ui {

class GmmStatusBar : public QWidget {
    Q_OBJECT
public:
    explicit GmmStatusBar(QWidget* parent = nullptr);

    void set_status(const QString& text);
    void set_nexus_info(const QString& info);
    void set_plugin_count(int count);

private:
    QHBoxLayout* layout_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* nexus_label_ = nullptr;
    QLabel* plugin_label_ = nullptr;
};

}  // namespace ui
