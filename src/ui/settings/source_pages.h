#pragma once

#include <QDialog>
#include <QString>

class QPlainTextEdit;
class QWidget;

namespace engine {
class SourceProvider;
}

namespace ui {

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
