#pragma once

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

// The archives the instance draws files from: every .bsa/.ba2 the ENABLED
// Bethesda plugin files load, marked when the archive is not on disk. MO2
// builds the same list from the loaded plugins' archives
// (mainwindow.cpp:1922-2090).
class ArchivesTab : public QWidget {
  Q_OBJECT
public:
  explicit ArchivesTab(QWidget *parent = nullptr);
  [[nodiscard]] QTreeWidget *tree() const { return tree_; }

  // Replace the listing. A name in `missing` keeps its row and is flagged -
  // a missing archive is exactly what this tab is for.
  void set_archives(const QStringList &names, const QStringList &missing);

private:
  QTreeWidget *tree_ = nullptr;
};

}  // namespace ui
