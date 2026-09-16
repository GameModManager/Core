#pragma once

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

#include "engine/gmmpack/types.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

namespace ui {

// Modpack install wizard framework. Left sidebar (step list, ~30%) + right
// QStackedWidget content + Cancel/Back/Next navigation. Step widgets are
// placeholders until Workspace-on1c implements the real content.
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

private:
    struct StepState {
        Step id;
        QString title;
        bool skipped = false;
        bool visited = false;
    };

    void build_steps();
    void go_to(int index);
    void on_next();
    void on_back();
    void on_cancel();
    void on_step_clicked(QListWidgetItem* item);
    void refresh_chrome();
    static QString step_title(Step step);

    engine::gmmpack::Gmmpack pack_;
    Mode mode_ = Mode::Append;
    std::vector<StepState> steps_;
    int current_ = 0;

    QListWidget* sidebar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPushButton* back_button_ = nullptr;
    QPushButton* next_button_ = nullptr;
};

}  // namespace ui
