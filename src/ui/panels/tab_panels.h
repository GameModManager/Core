#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QCheckBox;
class QProgressBar;
class QTableWidget;
class QTableWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;

namespace ui {

struct ModEntry;
struct ConflictPairs;

enum class DownloadState {
    Downloading,
    Complete,
    Installed,
    Failed
};

class DownloadsTab : public QWidget {
    Q_OBJECT
public:
    explicit DownloadsTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }

    void add_download(const std::string& id, const std::string& name,
                      const std::string& source,
                      const std::filesystem::path& file_path = {});
    void update_progress(const std::string& id, int64_t downloaded,
                         int64_t total, double speed);
    void mark_complete(const std::string& id, bool success);
    void mark_installed(const std::string& id);

    // Re-apply the "hide installed" filter on top of any other row filter.
    void reapply_installed_filter();

    // Persistence
    [[nodiscard]] std::string serialize() const;
    void deserialize(const std::string& json,
                     const std::filesystem::path& downloads_dir);

signals:
    void install_requested(const std::string& id,
                           const std::filesystem::path& file_path);

private:
    struct DownloadEntry {
        int row = -1;
        std::filesystem::path file_path;
        DownloadState state = DownloadState::Downloading;
        int64_t total_size = 0;
        QTableWidgetItem* name_item = nullptr;
        QTableWidgetItem* source_item = nullptr;
        QTableWidgetItem* size_item = nullptr;
        QProgressBar* progress_bar = nullptr;
    };

    DownloadEntry& entry_for(const std::string& id);
    void replace_bar_with_label(const std::string& id, const QString& text,
                                const QColor& bg);
    void on_cell_double_clicked(int row, int column);
    void apply_installed_filter();

    QTableWidget* table_ = nullptr;
    QCheckBox* hide_installed_ = nullptr;
    std::unordered_map<std::string, DownloadEntry> downloads_;
    int next_row_ = 0;
};

class PluginsTab : public QWidget {
    Q_OBJECT
public:
    explicit PluginsTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class ArchivesTab : public QWidget {
    Q_OBJECT
public:
    explicit ArchivesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class DataTab : public QWidget {
    Q_OBJECT
public:
    explicit DataTab(QWidget* parent = nullptr);
    [[nodiscard]] QTreeWidget* tree() const { return tree_; }

    // Populate the merged game-visible file tree from the conflict registry.
    //   registry          - relative path -> (mod_id, priority) providers
    //   all_mods          - current mod list (for display names)
    //   conflict_reversed - true if lower priority wins (Isaac convention)
    //   mods_dir          - instance mods dir (first place to stat winners)
    //   game_mods_dir     - game-native mods dir fallback (may be empty)
    void show_data(
        const std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>& registry,
        const QVector<ModEntry>& all_mods,
        bool conflict_reversed,
        const std::filesystem::path& mods_dir,
        const std::filesystem::path& game_mods_dir);

    void clear_content();

private:
    QTreeWidget* tree_ = nullptr;
};

class SavesTab : public QWidget {
    Q_OBJECT
public:
    explicit SavesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

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
