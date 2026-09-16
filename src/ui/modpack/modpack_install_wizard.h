#pragma once

#include <QDialog>
#include <QMap>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "engine/gmmpack/types.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QLineEdit;
class QProgressBar;
class QTableWidget;
class QLabel;
class QTimer;

namespace ui {

// Modpack install wizard: left sidebar (step list, ~30%) + right
// QStackedWidget content + Cancel/Back/Next navigation.
//
// Steps: Intro (pack info), Paths (new-instance dirs, skipped in append
// mode), Choices (choice groups), INI Tweaks, Downloads, Run Tools,
// Patches (binary-patch consent), Finishing (apply checklist), End
// (author closing message).
//
// Choice selections, INI toggles and patch consent live in the wizard so
// they persist while the user navigates back and forth.
class ModpackInstallWizard : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Append, CreateNew };
    enum class Step {
        Intro,
        Paths,
        Choices,
        IniTweaks,
        Downloads,
        RunTools,
        Patches,
        Finishing,
        End,
    };

    ModpackInstallWizard(engine::gmmpack::Gmmpack pack, Mode mode,
                         QWidget* parent = nullptr);

    [[nodiscard]] const engine::gmmpack::Gmmpack& pack() const { return pack_; }
    [[nodiscard]] Mode mode() const { return mode_; }

    // Step state, persisted across navigation.
    [[nodiscard]] QMap<QString, QStringList> choice_selections() const {
        return choice_selections_;
    }
    [[nodiscard]] QMap<QString, bool> ini_enabled() const { return ini_enabled_; }
    [[nodiscard]] QMap<QString, bool> patch_allowed() const {
        return patch_allowed_;
    }
    [[nodiscard]] QMap<QString, QString> download_status() const {
        return download_status_;
    }
    [[nodiscard]] QString instance_root() const;
    [[nodiscard]] QString mods_dir() const;
    [[nodiscard]] QString downloads_dir() const;
    [[nodiscard]] QString profile_path() const;

private:
    struct StepState {
        Step id;
        QString title;
        bool skipped = false;
        bool visited = false;
    };

    void build_steps();
    void build_pages();
    void go_to(int index);
    void on_next();
    void on_back();
    void on_cancel();
    void on_step_clicked(QListWidgetItem* item);
    void refresh_chrome();
    static QString step_title(Step step);

    // Page builders (one per step, in step order).
    QWidget* build_intro_page();
    QWidget* build_paths_page();
    QWidget* build_choices_page();
    QWidget* build_ini_page();
    QWidget* build_downloads_page();
    QWidget* build_run_tools_page();
    QWidget* build_patches_page();
    QWidget* build_finishing_page();
    QWidget* build_end_page();

    // Per-page refresh / enter hooks.
    void on_page_entered(int index);
    void refresh_downloads_ui();
    void refresh_next_enabled();
    void start_finishing_animation();
    [[nodiscard]] QString mod_display_name(const std::string& id) const;
    [[nodiscard]] bool downloads_complete() const;
    [[nodiscard]] bool choices_complete() const;

private slots:
    void on_browse_path(QLineEdit* edit);
    void on_choice_toggled();
    void on_ini_toggled();
    void on_download_one();
    void on_download_skip_optional();
    void on_run_tool_one();
    void on_run_tool_skip();
    void on_patch_allow_all();
    void on_patch_deny_all();
    void on_patch_cell_changed(int row, int column);
    void on_download_sim_tick();
    void on_tool_sim_tick();
    void on_finishing_tick();

private:
    engine::gmmpack::Gmmpack pack_;
    Mode mode_ = Mode::Append;
    std::vector<StepState> steps_;
    int current_ = 0;

    QListWidget* sidebar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* next_button_ = nullptr;

    // Step 2: paths.
    QLineEdit* instance_root_edit_ = nullptr;
    QLineEdit* mods_dir_edit_ = nullptr;
    QLineEdit* downloads_dir_edit_ = nullptr;
    QLineEdit* profile_path_edit_ = nullptr;

    // Steps 3/4/5/7: persisted user decisions.
    QMap<QString, QStringList> choice_selections_;  // group id -> mod ids
    QMap<QString, bool> ini_enabled_;               // tweak id -> enabled
    QMap<QString, QString> download_status_;        // mod id -> pending|downloading|downloaded|failed|skipped
    QMap<QString, bool> patch_allowed_;             // patch key -> allowed

    // Step 5 widgets (refreshed live).
    QProgressBar* downloads_bar_ = nullptr;
    QTableWidget* downloads_table_ = nullptr;

    // Step 6 widgets.
    QTableWidget* tools_table_ = nullptr;
    QLabel* tools_status_ = nullptr;
    QMap<QString, QString> tool_status_;  // exe id -> pending|running|done|skipped
    QString tool_running_id_;
    int tool_sim_ticks_ = 0;

    // Step 7 widgets.
    QTableWidget* patches_table_ = nullptr;

    // Step 8 widgets.
    QTableWidget* finishing_table_ = nullptr;
    QMap<int, bool> finishing_done_;
    int finishing_tick_ = 0;
    bool finishing_started_ = false;

    // Simulated async progress (real download/run pipelines wire in later).
    QTimer* sim_timer_ = nullptr;
    QString sim_mod_id_;
    int sim_ticks_ = 0;
};

}  // namespace ui
