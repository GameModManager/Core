#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVector>
#include <QPair>
#include <QJsonObject>
#include <QTemporaryDir>
#include <filesystem>

class QLineEdit;
class QComboBox;
class QListWidget;
class QDialogButtonBox;
class QModelIndex;
class QToolButton;

namespace ui {

struct ExecEntry {
    QString path;       // relative path from game_dir
    QString title;      // display name (empty = derive from path filename)
    QString arguments;  // CLI arguments
    QString start_in;   // working directory (empty = game_dir)
    QString output_mod; // mod ID for output routing (empty = none)
    QString icon_path;  // custom icon path (empty = auto-detect from binary)

    QJsonObject toJson() const;
    static ExecEntry fromJson(const QJsonObject& obj);
    static ExecEntry fromLegacyPath(const QString& relPath);
};

// Display name for a list row / combo item: explicit title, else the binary
// filename, else "Untitled". Shared by the dialog, the combo bar and logging.
QString exec_entry_display_name(const ExecEntry& e);

class ExecEntryDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExecEntryDialog(const std::filesystem::path& game_dir,
                              const QVector<QPair<QString, QString>>& mod_list,
                              const QVector<ExecEntry>& initial_entries,
                              const std::filesystem::path& icon_cache_dir = {},
                              QWidget* parent = nullptr);

    [[nodiscard]] QVector<ExecEntry> entries() const;

private:
    enum { InvalidIndex = -1 };

    void rebuild_list();
    void select_entry(int index);
    void save_current_entry();
    void on_add_from_file();
    void on_add_empty();
    void on_clone_selected();
    void on_remove_entry();
    void on_up_clicked();
    void on_down_clicked();
    void move_entry(int from, int to);
    void on_list_selection_changed();
    void on_field_changed();
    void browse_binary();
    void browse_start_in();
    void on_change_icon();
    void on_use_app_icon_toggled(bool checked);
    bool validate();

    // Reorders entries_ to match the list after a drag-drop move.
    void on_rows_about_to_move(const QModelIndex& parent, int start, int end,
                               const QModelIndex& destination, int row);
    void on_rows_moved(const QModelIndex& parent, int start, int end,
                       const QModelIndex& destination, int row);

    // Appends a new entry (deduped title), selects it, updates move buttons.
    void add_new_entry(const ExecEntry& entry);
    QString make_non_conflicting_title(const QString& base) const;
    void update_move_buttons();
    void restamp_list_indices();

    std::filesystem::path game_dir_;
    std::filesystem::path icon_cache_dir_;
    QVector<ExecEntry> entries_;
    int current_index_ = InvalidIndex;

    QListWidget* entry_list_ = nullptr;
    QToolButton* add_btn_ = nullptr;
    QToolButton* remove_btn_ = nullptr;
    QToolButton* up_btn_ = nullptr;
    QToolButton* down_btn_ = nullptr;

    QLineEdit* title_edit_ = nullptr;
    QLineEdit* binary_edit_ = nullptr;
    QLineEdit* args_edit_ = nullptr;
    QLineEdit* start_in_edit_ = nullptr;
    QComboBox* output_mod_combo_ = nullptr;
    QCheckBox* use_app_icon_check_ = nullptr;
    QPushButton* change_icon_btn_ = nullptr;
    QLabel* icon_preview_ = nullptr;

    QDialogButtonBox* buttons_ = nullptr;
    bool updating_fields_ = false;
    bool reordering_ = false;
};

}  // namespace ui
