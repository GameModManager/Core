#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

#include <vector>

class QListWidget;
class QPushButton;

namespace ui {

// MO2's ESPsTab: plugins (esp/esm/esl) in the mod's Data dir. A plugin at the
// Data root is active; one in a subfolder (MO2's "optional/") is inactive.
// Activate moves the file to the Data root (prompting on name clash),
// deactivate moves it to optional/ (or back where it came from).
class EspsTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit EspsTab(QWidget* parent = nullptr);
    ~EspsTab() override;

    void set_mod(const ModInfoData& data) override;

private:
    struct Esp {
        QString root_path;    // data dir
        QString active_path;  // relative to root, when active
        QString inactive_path;// relative to root, when inactive (may be empty)
        QString filename;

        bool is_active() const { return !active_path.isEmpty(); }
        bool has_inactive_path() const { return !inactive_path.isEmpty(); }
    };

    void rebuild();
    void on_activate();
    void on_deactivate();
    void repopulate(const QString& focus_active, const QString& focus_inactive);
    int index_of(const QListWidget* list, const QString& path) const;

    QListWidget* active_list_ = nullptr;
    QListWidget* inactive_list_ = nullptr;
    std::vector<Esp> esps_;
    QString data_dir_;
};

}  // namespace ui
