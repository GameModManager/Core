#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

namespace engine::Source {
class Interface;
}

namespace engine {
using SourceProvider = Source::Interface;
struct ModInfoResult;
}

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTextBrowser;

namespace ui {

class SourceFetchThread;

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
    void launch_fetch();
    void on_fetch_finished(engine::ModInfoResult result, quint64 generation);
    void apply_fetch_result(const engine::ModInfoResult& result);
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

    // Async Refresh (P8.3): the fetch runs on a worker thread
    // (gmm-source-fetch). `refresh_generation_` tags each launch so a stale
    // result (superseded by a newer Refresh) is dropped; `refresh_mod_id_`
    // pins the mod the fetch belongs to so a result can't land on a different
    // mod after a prev/next switch. `fetch_in_flight_`/`refresh_pending_`
    // coalesce rapid Re-clicks into at most one queued follow-up.
    SourceFetchThread* source_fetch_thread_ = nullptr;
    quint64 refresh_generation_ = 0;
    QString refresh_mod_id_;
    bool fetch_in_flight_ = false;
    bool refresh_pending_ = false;
};

}  // namespace ui
