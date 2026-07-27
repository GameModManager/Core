#pragma once

#include <QTreeView>

namespace ui {

class ModTableView : public QTreeView {
    Q_OBJECT
public:
    explicit ModTableView(QWidget* parent = nullptr);
};

}  // namespace ui
