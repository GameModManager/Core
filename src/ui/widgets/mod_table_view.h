#pragma once

#include <QTreeView>
#include <QStringList>

namespace ui {

class ModTableView : public QTreeView {
    Q_OBJECT
public:
    explicit ModTableView(QWidget* parent = nullptr);

signals:
    void files_dropped(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

}  // namespace ui
