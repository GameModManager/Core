#include "ui/modinfo/config_files_tab.h"

#include <QFileInfo>

namespace ui {

ConfigFilesTab::ConfigFilesTab(QWidget* parent) : GenericFilesTab(parent) {}

bool ConfigFilesTab::wants_file(const QString& rel_path,
                                const QString& full_path) const {
    static const QStringList kExtensions = {
        QStringLiteral(".ini"), QStringLiteral(".cfg"),
        QStringLiteral(".toml"), QStringLiteral(".yaml"),
        QStringLiteral(".yml"), QStringLiteral(".json"),
    };
    const QString suffix = QFileInfo(full_path).suffix().toLower();
    if (suffix.isEmpty()) return false;
    if (!kExtensions.contains(QLatin1Char('.') + suffix)) return false;
    // The mod's own meta.ini is manager data, not a game config.
    return !rel_path.endsWith(QStringLiteral("meta.ini"), Qt::CaseInsensitive);
}

}  // namespace ui
