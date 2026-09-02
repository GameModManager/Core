#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

class QTabWidget;

namespace ui {

// MO2's Nexus tab generalized into a per-mod Source tab. Shows exactly ONE
// source panel (the mod's actual source from meta) plus a "+" tab on the
// right that lets the user add another source manually. The previous
// "show every game-supported source" behavior fabricated Nexus presence on
// every mod (Workspace-fqf5): a manual / Steam / LoversLab mod under a
// Nexus-enabled game used to display a Nexus tab even though no Nexus
// provenance existed, and users conflated tab visibility with actual
// source attribution.
class SourceTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit SourceTab(QWidget* parent = nullptr);
    ~SourceTab() override;

    void set_mod(const ModInfoData& data) override;
    void first_activation() override;
    void save_state() override;

private:
    // Build (or rebuild) the single source tab plus the "+" affordance tab
    // from the current ModInfoData and the mod's meta. Called whenever the
    // displayed mod changes or after the user attaches a new source via "+".
    void populate();

    // Open a modal dialog that lets the user attach a Nexus / LoversLab /
    // Steam source to the current mod. On confirm, writes the appropriate
    // provider section + [GameModManager]source_type/source_id via
    // current().save_meta(), updates the in-memory ModInfoData, then
    // repopulates the tab.
    void show_add_source_dialog();

    QTabWidget* sources_ = nullptr;
    // Index of the "+" tab inside sources_, -1 when none. Stored so the
    // currentChanged handler can detect when the user tried to activate it
    // and intercept (open the dialog) without leaving the tab focused.
    int plus_index_ = -1;
};

}  // namespace ui
