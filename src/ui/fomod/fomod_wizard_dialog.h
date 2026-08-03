#pragma once

#include "engine/fomod/fomod_view_model.h"
#include "engine/pipeline/pipeline.h"

#include <QDialog>
#include <QHash>
#include <QString>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSplitter;
class QStackedWidget;

namespace ui {

class ScaledImageLabel;

// FOMOD install wizard, ported from FOMOD Plus FomodInstallerWindow (MIT).
//
// The dialog drives the pipeline-owned engine::FomodViewModel directly - the
// pipeline worker is blocked inside the modal for the whole lifetime, so all
// mutation is single-threaded. On accept it hands back the selected options in
// the FOMOD Plus fomod.json shape, the (possibly edited) mod name, and whether
// sources missing from the archive may be skipped; on Manual it reports
// manual_install() so the engine installs the archive contents as-is.
class FomodWizardDialog final : public QDialog {
    Q_OBJECT
public:
    FomodWizardDialog(const std::shared_ptr<engine::FomodViewModel>& view_model,
                      const std::filesystem::path& content_root,
                      QString suggested_name, QString previous_choices,
                      bool always_restore_choices, bool show_images,
                      QWidget* parent = nullptr);

    void done(int result) override;

    [[nodiscard]] bool manual_install() const { return manual_; }
    [[nodiscard]] bool ignore_missing() const { return ignore_missing_; }
    [[nodiscard]] QString mod_name() const;
    [[nodiscard]] std::string choices_json() const;

private:
    struct PluginData {
        std::shared_ptr<engine::PluginViewModel> plugin;
        QAbstractButton* ui_element = nullptr;
    };

    // Buttons
    QPushButton* next_install_button_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* manual_button_ = nullptr;
    QPushButton* select_previous_button_ = nullptr;
    QPushButton* reset_choices_button_ = nullptr;

    // Widgets
    QComboBox* name_input_ = nullptr;
    QLabel* description_box_ = nullptr;
    QSplitter* center_row_ = nullptr;
    QSplitter* left_pane_ = nullptr;
    QStackedWidget* step_stack_ = nullptr;
    ScaledImageLabel* image_label_ = nullptr;

    // Model
    std::shared_ptr<engine::FomodViewModel> view_model_;
    std::filesystem::path content_root_;
    QString suggested_name_;
    QString previous_choices_;
    bool always_restore_choices_ = false;
    bool show_images_ = false;
    bool manual_ = false;
    bool ignore_missing_ = false;
    QHash<QString, PluginData> plugin_map_;

    // Construction
    void setup_ui();
    void update_buttons();
    void update_step_stack();
    void update_display_for_active_plugin();
    QWidget* create_top_row();
    QWidget* create_bottom_row();
    QSplitter* create_center_row();
    QSplitter* create_left_pane();
    QWidget* create_right_pane();
    QComboBox* create_name_combo();
    QWidget* create_step_widget(const std::shared_ptr<engine::StepViewModel>& step);
    QWidget* render_group(engine::GroupRef group, const std::shared_ptr<engine::StepViewModel>& step);
    QRadioButton* create_plugin_radio(engine::PluginRef plugin, engine::GroupRef group,
                                      const std::shared_ptr<engine::StepViewModel>& step, QWidget* parent);
    QCheckBox* create_plugin_checkbox(engine::PluginRef plugin, engine::GroupRef group,
                                      const std::shared_ptr<engine::StepViewModel>& step, QWidget* parent);
    void render_select_exactly_one(QWidget* parent, QLayout* parent_layout, engine::GroupRef group,
                                   const std::shared_ptr<engine::StepViewModel>& step);
    void render_checkbox_group(QWidget* parent, QLayout* parent_layout, engine::GroupRef group,
                               const std::shared_ptr<engine::StepViewModel>& step);
    QButtonGroup* render_radio_group(QWidget* parent, QLayout* parent_layout, engine::GroupRef group,
                                     const std::shared_ptr<engine::StepViewModel>& step);
    void update_could_be_usable_indicator(QAbstractButton* button, engine::PluginRef plugin);
    void show_image_viewer();

    // Previous-choice restore/styling
    void apply_fn_from_json(const std::string& plugin_selector,
                            const std::function<void(QAbstractButton*)>& fn);
    void style_previously_selected_options();
    void style_previously_deselected_options();
    void select_previously_selected_options();

    // View-model sync
    void populate_plugin_map();
    void update_checkbox_states();
    void on_plugin_toggled(bool selected, engine::GroupRef group, engine::PluginRef plugin);
    void on_plugin_manually_unchecked(engine::PluginRef plugin);
    void on_plugin_hovered(engine::PluginRef plugin);

    // Actions
    void on_next_clicked();
    void on_back_clicked();
    void on_manual_clicked();
    void on_cancel_clicked();
    void on_install_clicked();
    void on_reset_choices_clicked();
    void save_geometry_and_state();
    void restore_geometry_and_state();

    static QString object_name_for(engine::PluginRef plugin, engine::GroupRef group);
};

// Blocking helper that runs the wizard and returns the full decision. Safe to
// call from any thread: when invoked off the main thread (the install pipeline
// runs on a worker QThread) it marshals the modal dialog onto the main thread
// and waits, using the same pattern as ui::ask_overwrite.
engine::FomodDecision ask_fomod(const std::shared_ptr<engine::FomodViewModel>& view_model,
                                const std::filesystem::path& content_root,
                                const std::string& suggested_name,
                                const std::string& previous_choices,
                                bool always_restore_choices, bool show_images,
                                QWidget* parent = nullptr);

}  // namespace ui
