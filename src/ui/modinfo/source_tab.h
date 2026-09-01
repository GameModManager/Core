#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

class QTabWidget;

namespace ui {

// MO2's Nexus tab generalized into a per-game Source tab: one sub-tab per
// source the current game supports (the download_sources knowledge key), each
// built from 2-column QFormLayout rows like the Settings > Sources pages.
// Nexus gets the full metadata form; Steam gets the Steam Workshop form;
// other providers get a minimal identity page.
class SourceTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit SourceTab(QWidget* parent = nullptr);
    ~SourceTab() override;

    void set_mod(const ModInfoData& data) override;
    void first_activation() override;
    void save_state() override;

private:
    void populate();

    QTabWidget* sources_ = nullptr;
};

}  // namespace ui
