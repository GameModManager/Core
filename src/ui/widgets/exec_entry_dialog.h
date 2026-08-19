#pragma once

#include <QDialog>
#include <QPair>
#include <QVector>
#include <filesystem>

namespace ui {

class ExecEntryContentWidget;
struct ExecEntry;

// Executable editor dialog. Thin QDialog wrapper around the mode-agnostic
// ExecEntryContentWidget: the editor plus Save/Cancel mapped to accept/reject.
// In Full UI tab mode the same widget is embedded directly in
// MainTabContainer, so this dialog is only used for popup mode.
class ExecEntryDialog : public QDialog {
  Q_OBJECT
public:
  explicit ExecEntryDialog(const std::filesystem::path &game_dir,
                           const QVector<QPair<QString, QString>> &mod_list,
                           const QVector<ExecEntry> &initial_entries,
                           const std::filesystem::path &icon_cache_dir = {},
                           QWidget *parent = nullptr);

  [[nodiscard]] QVector<ExecEntry> entries() const;

private:
  ExecEntryContentWidget *content_ = nullptr;
};

} // namespace ui
