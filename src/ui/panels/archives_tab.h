#pragma once

#include <QWidget>

class QTreeWidget;

namespace ui {

class ArchivesTab : public QWidget {
    Q_OBJECT
public:
    explicit ArchivesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTreeWidget* tree() const { return tree_; }
private:
    QTreeWidget* tree_ = nullptr;
};

}  // namespace ui
