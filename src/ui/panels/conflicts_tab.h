#pragma once

#include <QMap>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include <string>
#include <unordered_map>
#include <vector>

class QMenu;
class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

struct ModEntry;
struct ConflictPairs;

class ConflictsTab : public QWidget {
    Q_OBJECT
public:
    explicit ConflictsTab(QWidget* parent = nullptr);

    void show_conflicts(
        const QString& selected_mod_id,
        const QVector<ModEntry>& all_mods,
        const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& file_registry,
        const QMap<QString, ConflictPairs>& pairs,
        bool conflict_reversed);

    void clear_content();

signals:
    void file_open_requested(const QString& mod_id, const QString& relative_path);
    void image_diff_requested(const QString& relative_path);

private:
    void on_item_double_clicked(QTreeWidgetItem* item, int column);
    void on_custom_context_menu(const QPoint& pos);
    void on_merge_in_imagediff();

    QTreeWidget* tree_ = nullptr;
    QString context_file_path_;
};

}  // namespace ui
