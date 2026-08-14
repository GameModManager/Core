#pragma once

#include "engine/source/nexus_auth.h"

#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QWidget;

namespace engine {
class SourceProvider;
}

namespace ui {

// Tier-derived default for the "Queue downloads (one at a time)" Nexus option.
// Regular/Supporter accounts keep the free-account ~1.5MB/s throttle, so they
// default to queueing; Premium lifts the cap, so it defaults to parallel.
bool nexus_queue_default_for(engine::NexusUserInfo::AccountType type);

// Applied on a successful login (users/validate.json): writes the tier-derived
// default into Settings ONLY while the user has never set the value explicitly,
// so a manual checkbox choice survives later logins. Declared here so the
// settings tests can drive it without opening the modal.
void apply_nexus_queue_default();

// Modal "Enter API Key Manually" dialog (MO2's NexusManualKeyDialog): a
// fixed-font multiline key field with Open Browser / Paste / Clear buttons.
// Declared here (not file-local) so the settings tests can drive it without
// opening the modal: set the text via the child QPlainTextEdit and call
// accept(), then read key().
class NexusManualKeyDialog : public QDialog {
    Q_OBJECT
public:
    explicit NexusManualKeyDialog(QWidget* parent = nullptr);
    QString key() const;
    void accept() override;

private:
    void open_browser();
    void paste();
    void clear();

    QPlainTextEdit* key_edit_ = nullptr;
    QString key_;
};

// Build the settings page for a source provider, embedded in the Sources tab
// of the settings dialog. Each known source type contributes its own page
// here (UI layer only — engine providers stay Qt-free so the headless tests
// can compile them). Returns nullptr if the provider has nothing to configure.
QWidget* build_source_settings_page(engine::SourceProvider* provider,
                                    QWidget* parent);

} // namespace ui
