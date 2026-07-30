#pragma once

#include <QWidget>

#include <filesystem>
#include <string>
#include <unordered_map>

class QProgressBar;
class QTableWidget;
class QTableWidgetItem;

namespace ui {

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

    QTableWidget* table_ = nullptr;
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
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
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
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

}  // namespace ui
