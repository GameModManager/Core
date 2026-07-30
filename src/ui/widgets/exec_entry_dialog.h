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
    void on_add_entry();
    void on_remove_entry();
    void on_list_selection_changed();
    void on_field_changed();
    void browse_binary();
    void browse_start_in();
    void on_change_icon();
    void on_use_app_icon_toggled(bool checked);
    bool validate();

    static QString display_name(const ExecEntry& e);

    std::filesystem::path game_dir_;
    std::filesystem::path icon_cache_dir_;
    QVector<ExecEntry> entries_;
    int current_index_ = InvalidIndex;

    QListWidget* entry_list_ = nullptr;

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
};

}  // namespace ui
