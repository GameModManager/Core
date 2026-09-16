#pragma once

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;

namespace ui {

// One matching instance for the instance-selection dropdown.
struct ModpackInstanceChoice {
    std::string folder_name;   // instance dir basename (stable id)
    std::string display_name;  // user-facing name
    std::string game_id;       // GMM canonical game id from instance.toml
};

// Modal shown after pack source detection: pick which instance the pack
// installs into. Append skips the wizard Paths step; Create New keeps it.
class ModpackInstanceDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Cancelled, Append, CreateNew };

    ModpackInstanceDialog(const std::string& pack_game_id,
                          const QString& pack_game_display,
                          std::vector<ModpackInstanceChoice> instances,
                          const std::string& active_folder_name,
                          QWidget* parent = nullptr);

    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] std::string selected_folder() const;

private:
    QComboBox* combo_ = nullptr;
    QLabel* empty_label_ = nullptr;
    QPushButton* append_button_ = nullptr;
    std::vector<ModpackInstanceChoice> instances_;
    Mode mode_ = Mode::Cancelled;
};

}  // namespace ui
