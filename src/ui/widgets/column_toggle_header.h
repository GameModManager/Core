#pragma once

#include <QHeaderView>

namespace ui {

class ColumnToggleHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit ColumnToggleHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr);

    void set_column_labels(const QStringList& labels);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuild_menu();
    QStringList labels_;
};

}  // namespace ui
