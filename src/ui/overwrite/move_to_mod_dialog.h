#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ui/widgets/list_dialog.h"

namespace ui {

// MO2 "Move content to Mod..." destination picker. Wraps the shared ListDialog
// (MO2 modlistviewactions.cpp:1440 uses the same ListDialog as the separator
// picker): the caller-filtered mod list (separators / Overwrite / MERGED /
// game-native are excluded by the caller); the chosen mod receives ALL
// Overwrite contents.
class MoveToModDialog : public ListDialog {
    Q_OBJECT
public:
    // mods = (folder_name, display_name) for eligible mods.
    explicit MoveToModDialog(
        const std::vector<std::pair<std::string, std::string>>& mods,
        QWidget* parent = nullptr);

    // Chosen mod folder, or empty if cancelled/nothing selected.
    std::string selected_folder() const;
};

}  // namespace ui
