#pragma once

#include "ui/modinfo/mod_info_tab.h"

namespace engine {
class SourceProvider;
}

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTextBrowser;

namespace ui {

// MO2's Nexus tab generalized into a per-game Source tab: one sub-tab per
// source the current game supports (the download_sources knowledge key), each
// built from 2-column QFormLayout rows like the Settings > Sources pages.
// Nexus gets the full metadata form (mod id, source game, version, category,
// description, Refresh/Visit/custom URL); other providers get a minimal
// identity page (source name + source id). Each source's data persists under
// the corresponding section header in the mod's ini ([Nexusmods], ...).
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
    void update_version_color();
    void render_description();
    void on_refresh();
    void on_visit();
    void on_visit_custom();
    void on_custom_url_toggled();
    void persist_fields();
    void persist_custom_url();
    QString meta_value(const char* section, const char* key) const;
    void set_meta_value(const char* section, const char* key, const QString& v);

    QWidget* build_nexus_page(QWidget* parent);
    QWidget* build_generic_page(engine::SourceProvider* provider,
                                QWidget* parent);

    QTabWidget* sources_ = nullptr;

    QLineEdit* mod_id_ = nullptr;
    QComboBox* source_game_ = nullptr;
    QLineEdit* version_ = nullptr;
    QLineEdit* category_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* visit_ = nullptr;
    QCheckBox* custom_url_toggle_ = nullptr;
    QLineEdit* custom_url_ = nullptr;
    QPushButton* visit_custom_ = nullptr;
    QTextBrowser* description_ = nullptr;
    bool loading_ = false;
};

}  // namespace ui
