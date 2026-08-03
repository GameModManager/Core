#include "engine/theme/style_manager.h"
#include "engine/theme/theme_manager.h"
#include "engine/log/logger.h"

#include <QApplication>
#include <QFile>
#include <QString>

namespace engine {

// -- Embedded default theme ---------------------------------------------
// Uses palette() exclusively so the desktop provides all colors - no
// hardcoded tints. This is the fallback when no user theme is loaded.
// Minimal QSS: only app-specific named-widget rules.
// Everything else uses QPalette + native KDE style.
static const char* default_qss = R"(
QToolTip {
    color: palette(toolTipText);
    background-color: palette(toolTipBase);
}

/* -- Game lock overlay --------------------------- */

#gameLockOverlay {
    background-color: rgba(0, 0, 0, 160);
    color: white;
}

#gameLockOverlay QLabel {
    color: white;
    font-size: 20px;
}

#gameLockOverlay QPushButton {
    font-size: 14px;
}

#unlockBtn:hover {
    background-color: palette(highlight);
}

#killBtn {
    background-color: #8b0000;
    color: white;
}

#killBtn:hover {
    background-color: #a00000;
}

#gameLockOverlay QTreeWidget {
    color: white;
    background-color: rgba(0, 0, 0, 180);
}

#gameLockOverlay QTreeWidget::item {
    background-color: transparent;
}

#gameLockOverlay QTreeWidget::item:selected {
    color: white;
    background-color: rgba(255, 255, 255, 30);
}

#gameLockOverlay QHeaderView::section {
    color: white;
    background-color: rgba(50, 50, 50, 200);
}

#processTreeLabel {
    color: white;
}

/* -- Instance switcher dialog -------------------- */

#pathLabel {
    color: palette(placeholderText);
    font-size: 10px;
}

/* -- Separator rows ------------------------------ */

QWidget#separatorRow {
    font-weight: bold;
}

/* -- Console panel ------------------------------- */

#consoleOutput {
    font-family: monospace;
}

/* -- Downloads tab ------------------------------- */

/* MO2 parity: row height is driven by ::item padding (QSS `height` is ignored
   for item views). The DownloadsTab sets the dynamic `compact` property and
   re-polishes; these scoped rules keep every other table native. */
QTableWidget#downloadsTable[compact="true"]::item {
    padding: 4px 6px;
}

QTableWidget#downloadsTable[compact="false"]::item {
    padding: 16px 6px;
}

/* -- Debug window -------------------------------- */

QLabel#debugKey {
    font-weight: bold;
}

QLabel#debugValue {
    font-family: monospace;
}

/* -- Statistics dialog -------------------------- */

QLabel#statLabel {
    font-weight: bold;
}

/* -- Pipeline window ---------------------------- */

#pipelineHeader {
    font-weight: bold;
}

/* Card sits on the scene's palette(base) canvas; the border delineates it. */
#pipelineCard {
    background-color: palette(base);
    border: 1px solid palette(midlight);
    border-radius: 4px;
}

#pipelineCardHeader {
    border-bottom: 1px solid palette(midlight);
    border-top-left-radius: 3px;
    border-top-right-radius: 3px;
}

#pipelineStageName {
    font-weight: bold;
}

#pipelineOrigin {
    color: palette(mid);
    font-size: 10px;
}

#pipelineStatus {
    font-size: 10px;
    font-weight: bold;
}

#pipelineDescription {
    font-size: 10px;
}

#pipelineDuration {
    color: palette(mid);
    font-family: monospace;
    font-size: 10px;
}

/* Failure message box - red border, ~10% red fill, whole message. */
#pipelineFailBox {
    border: 1px solid #c62828;
    background-color: rgba(198, 40, 40, 25);
    color: #c62828;
    border-radius: 3px;
    padding: 6px;
    margin: 0 8px 8px 8px;
    font-size: 10px;
}

#pipelinePlaceholder {
    color: palette(mid);
    font-style: italic;
}

/* Floating zoom bar pinned to the bottom-right of the pipeline canvas. */
#zoomControls {
    background-color: palette(midlight);
    border-radius: 6px;
}

#zoomPercent {
    color: palette(windowText);
    font-weight: bold;
    font-size: 10px;
    padding: 0 2px;
}

/* -- Game selection ------------------------------ */

#gameSelectionSubtitle {
    color: palette(mid);
}

#gameSelectionNoInstall {
    color: palette(mid);
    font-style: italic;
}
)";

StyleManager::StyleManager(ThemeManager& theme_manager, QObject* parent)
    : QObject(parent)
    , theme_manager_(theme_manager)
{
    connect(&watcher_, &QFileSystemWatcher::fileChanged,
            this, [this]() { reload_current(); });
}

StyleManager::~StyleManager() = default;

void StyleManager::apply_default() {
    current_theme_ = "default";
    current_qss_path_.clear();
    current_tokens_path_.clear();
    watcher_.removePaths(watcher_.files());

    // Clear any per-widget setStyleSheet remnants so the global sheet wins
    apply_qss(default_qss);
    engine::Logger::instance().debug("Applied default palette-based theme");
    emit theme_applied(QString::fromStdString(current_theme_));
}

bool StyleManager::apply_theme(const std::string& name) {
    if (name.empty() || name == "default") {
        apply_default();
        return false;
    }
    const auto* theme = theme_manager_.find_theme(name);
    if (!theme) {
        engine::Logger::instance().error("StyleManager: theme not found: " + name);
        apply_default();
        return false;
    }
    return load_theme(*theme);
}

bool StyleManager::load_theme(const ThemeManager::ThemeInfo& theme) {
    QFile f(QString::fromStdString(theme.qss_path.string()));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        engine::Logger::instance().error("StyleManager: cannot open theme: " +
            theme.qss_path.string());
        return false;
    }

    QString content = QString::fromUtf8(f.readAll());
    f.close();

    current_theme_ = theme.name;
    current_qss_path_ = theme.qss_path;
    current_tokens_path_ = theme.tokens_path;
    watch_theme_files();

    if (!theme.tokens_path.empty()) {
        theme_manager_.load_tokens(theme.tokens_path);
        std::string rendered = theme_manager_.apply_template(content.toStdString());
        apply_qss(rendered);
    } else {
        apply_qss(content.toStdString());
    }

    engine::Logger::instance().debug("Loaded theme: " + current_theme_ +
        " (" + std::to_string(content.size()) + " bytes)");
    emit theme_applied(QString::fromStdString(current_theme_));
    return true;
}

void StyleManager::reload_current() {
    if (current_theme_ == "default" || current_qss_path_.empty()) {
        apply_default();
        return;
    }
    ThemeManager::ThemeInfo info;
    info.name = current_theme_;
    info.qss_path = current_qss_path_;
    info.tokens_path = current_tokens_path_;
    load_theme(info);
}

std::vector<std::string> StyleManager::theme_names() const {
    std::vector<std::string> names;
    for (const auto& theme : theme_manager_.themes()) {
        names.push_back(theme.name);
    }
    return names;
}

void StyleManager::watch_theme_files() {
    watcher_.removePaths(watcher_.files());
    watcher_.addPath(QString::fromStdString(current_qss_path_.string()));
    if (!current_tokens_path_.empty()) {
        watcher_.addPath(QString::fromStdString(current_tokens_path_.string()));
    }
}

void StyleManager::apply_qss(const std::string& qss_content) {
    current_qss_ = qss_content;
    qApp->setStyleSheet(QString::fromStdString(qss_content));
}

} // namespace engine
