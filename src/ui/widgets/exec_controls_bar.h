#pragma once

#include <QWidget>
#include <QStringList>
#include <QJsonObject>
#include <filesystem>

#include "ui/widgets/exec_entry_dialog.h"

class QComboBox;
class QToolButton;

namespace ui {

// Sentinel item text shown in the executable combo (the "add new entry" slot).
// Kept in the header so main_window.cpp can compare against it without relying
// on a translated string. Display sites wrap it in tr().
inline constexpr const char* kAddNewEntryText = "<Edit...>";

class ExecControlsBar : public QWidget {
    Q_OBJECT
public:
    explicit ExecControlsBar(QWidget* parent = nullptr);

    void set_executables(const QStringList& names, const QString& default_name = {},
                          const std::filesystem::path& game_dir = {},
                          const std::filesystem::path& icon_cache_dir = {});
    void clear_executables();
    [[nodiscard]] QString current_executable() const;
    [[nodiscard]] int current_executable_index() const;
    [[nodiscard]] QStringList executable_paths() const;

    // Returns the full ExecEntry for each item (excluding the sentinel)
    [[nodiscard]] QVector<ExecEntry> executable_entries() const;

    // Returns the ExecEntry for the currently selected item
    [[nodiscard]] ExecEntry current_entry() const;

    // Legacy: adds an entry with just a path (no extra metadata)
    void add_executable(const QString& display_name, const QString& rel_path, const QIcon& icon = {});

    // New: adds a full entry with metadata
    void add_entry(const ExecEntry& entry);

signals:
    void run_clicked();
    void shortcut_to_toolbar();
    void shortcut_to_desktop();
    void add_entry_requested();
    void current_executable_changed();

private:
    QJsonObject item_data(int index) const;
    void set_item_data(int index, const QJsonObject& obj);

    QComboBox* exec_combo_ = nullptr;
    QToolButton* run_btn_ = nullptr;
    QToolButton* shortcut_btn_ = nullptr;
};

}  // namespace ui
