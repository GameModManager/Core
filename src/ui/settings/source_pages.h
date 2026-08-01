#pragma once

#include <string>

class QWidget;

namespace engine {
class SourceProvider;
}

namespace ui {

// Build the settings page for a source provider, embedded in the Sources tab
// of the settings dialog. Each known source type contributes its own page
// here (UI layer only — engine providers stay Qt-free so the headless tests
// can compile them). Returns nullptr if the provider has nothing to configure.
QWidget* build_source_settings_page(engine::SourceProvider* provider,
                                    QWidget* parent);

} // namespace ui
