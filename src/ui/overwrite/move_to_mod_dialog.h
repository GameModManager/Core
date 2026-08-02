#pragma once

#include <QDialog>
#include <string>
#include <utility>
#include <vector>

class QListWidget;

namespace ui {

// MO2 "Move content to Mod..." destination picker (ListDialog port). Shows the
// caller-filtered mod list (separators / Overwrite / MERGED / game-native are
// excluded by the caller); the chosen mod receives ALL Overwrite contents.
class MoveToModDialog : public QDialog {
    Q_OBJECT
public:
    // mods = (folder_name, display_name) for eligible mods.
    explicit MoveToModDialog(
        const std::vector<std::pair<std::string, std::string>>& mods,
        QWidget* parent = nullptr);

    // Chosen mod folder, or empty if cancelled/nothing selected.
    std::string selected_folder() const;

private:
    QListWidget* list_ = nullptr;
};

}  // namespace ui
