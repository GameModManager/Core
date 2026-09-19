#pragma once

#include <QDialog>
#include <QString>

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/core/instance/instance_snapshot.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;
class QLineEdit;
class QTextEdit;
class QTableWidget;
class QLabel;
class QProgressBar;
class QTreeWidget;

namespace ui {

// Modpack export wizard: left sidebar (step list, ~30%) + right
// QStackedWidget content + Cancel/Back/Next navigation.
//
// Steps: Info (pack metadata), Mods (include/category review),
// Executables (include selection), Tree (layout preview), Review
// (summary + output path + Export).
//
// The packer engine (engine::gmmpack) only exports what is in the
// snapshot it is given, so include choices are applied by handing
// create_gmmpack() a filtered snapshot copy. Per-row categories are
// stored for reference (the engine currently exports all mods as
// Optional).
class ExportWizard : public QDialog {
    Q_OBJECT
public:
    enum class Step {
        Info,
        Mods,
        Executables,
        Tree,
        Review,
    };

    ExportWizard(const engine::InstanceSnapshot& snapshot,
                 std::filesystem::path mods_dir, QWidget* parent = nullptr);
    ~ExportWizard() override = default;

private:
    struct StepState {
        Step id;
        QString title;
        bool skipped = false;
        bool visited = false;
    };

    struct ModRow {
        std::string folder;
        QString source;
        bool disabled = false;
        bool resolvable = true;
        bool included = true;
        bool is_vanilla = false;   // unmanaged game master (Skyrim.esm etc.)
        bool is_manual = false;    // manual/unknown source - shippable but opt-in
    };

    struct ExeRow {
        int index = -1;
        QString title;
        QString path;
        QString args;
        bool included = true;
    };

    void build_steps();
    void build_pages();
    void load_foreign_mods();
    void build_mod_rows();
    void build_exe_rows();
    void go_to(int index);
    void on_next();
    void on_back();
    void on_cancel();
    void on_step_clicked(QListWidgetItem* item);
    void refresh_chrome();
    static QString step_title(Step step);

    // Page builders (one per step, in step order).
    QWidget* build_info_page();
    QWidget* build_mods_page();
    QWidget* build_executables_page();
    QWidget* build_tree_page();
    QWidget* build_review_page();

    // Per-page refresh / enter hooks.
    void on_page_entered(int index);
    void refresh_mods_count();
    void refresh_tree();
    void refresh_review();
    [[nodiscard]] engine::InstanceSnapshot filtered_snapshot() const;
    [[nodiscard]] int separator_count() const;

private slots:
    void on_exclude_disabled();
    void on_mod_include_toggled();
    void on_exe_include_toggled();
    void on_browse_output();
    void on_export();

private:
    engine::InstanceSnapshot snapshot_;
    std::filesystem::path mods_dir_;
    std::unordered_set<std::string> foreign_mods_;  // unmanaged vanilla masters
    std::vector<StepState> steps_;
    int current_ = 0;
    std::vector<ModRow> mods_;
    std::vector<ExeRow> exes_;

    QListWidget* sidebar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* next_button_ = nullptr;

    // Step 1: pack metadata.
    QLineEdit* name_edit_ = nullptr;
    QLineEdit* author_edit_ = nullptr;
    QTextEdit* desc_edit_ = nullptr;
    QLineEdit* homepage_edit_ = nullptr;
    QTextEdit* instructions_edit_ = nullptr;

    // Step 2: mod review.
    QTableWidget* mods_table_ = nullptr;
    QLabel* mods_count_ = nullptr;

    // Step 3: executable selection.
    QTableWidget* exes_table_ = nullptr;

    // Step 4: tree preview.
    QTreeWidget* tree_ = nullptr;

    // Step 5: review + export.
    QLabel* review_summary_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QLabel* status_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
};

}  // namespace ui
