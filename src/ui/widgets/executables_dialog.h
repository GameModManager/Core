#pragma once

#include <QDialog>
#include <QPair>
#include <QVector>
#include <filesystem>

namespace ui {
namespace Executables {

class ContentWidget;
struct Entry;

// Executable editor dialog. Thin QDialog wrapper around the mode-agnostic
// ContentWidget: the editor plus Save/Cancel mapped to accept/reject.
// In Full UI tab mode the same widget is embedded directly in
// MainTabContainer, so this dialog is only used for popup mode.
class Dialog : public QDialog {
  Q_OBJECT
public:
  explicit Dialog(const std::filesystem::path &game_dir,
                  const QVector<QPair<QString, QString>> &mod_list,
                  const QVector<Entry> &initial_entries,
                  const std::filesystem::path &icon_cache_dir = {},
                  QWidget *parent = nullptr);

  [[nodiscard]] QVector<Entry> entries() const;

private:
  ContentWidget *content_ = nullptr;
};

} // namespace Executables
} // namespace ui
