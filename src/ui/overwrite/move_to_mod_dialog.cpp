#include "ui/overwrite/move_to_mod_dialog.h"

namespace ui {

MoveToModDialog::MoveToModDialog(
    const std::vector<std::pair<std::string, std::string>>& mods,
    QWidget* parent)
    : ListDialog(parent) {
    setWindowTitle(tr("Move content to Mod..."));
    setMinimumSize(360, 320);

    QStringList names;
    QList<QVariant> folders;
    for (const auto& [folder, name] : mods) {
        names << QString::fromStdString(name.empty() ? folder : name);
        folders << QString::fromStdString(folder);
    }
    setChoices(names);
    setChoiceData(folders);
    if (!names.isEmpty()) setCurrentRow(0);
}

std::string MoveToModDialog::selected_folder() const {
    return getChoiceData().toString().toStdString();
}

}  // namespace ui
