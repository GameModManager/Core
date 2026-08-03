#include "ui/modinfo/text_files_tab.h"

#include <QFileInfo>

namespace ui {

TextFilesTab::TextFilesTab(QWidget* parent) : GenericFilesTab(parent) {}

bool TextFilesTab::wants_file(const QString& rel_path,
                              const QString& full_path) const {
    Q_UNUSED(rel_path)
    // Config extensions (.ini/.cfg/.toml/.yaml/.yml/.json) live in the Config
    // Files tab; keep prose/scripts/docs here.
    static const QStringList kExtensions = {
        QStringLiteral(".txt"), QStringLiteral(".md"),  QStringLiteral(".readme"),
        QStringLiteral(".xml"), QStringLiteral(".log"),
        QStringLiteral(".lua"), QStringLiteral(".py"),
        QStringLiteral(".html"), QStringLiteral(".htm"), QStringLiteral(".css"),
        QStringLiteral(".js"),   QStringLiteral(".csv"), QStringLiteral(".bat"),
        QStringLiteral(".cmd"),  QStringLiteral(".sh"),  QStringLiteral(".ps1"),
        QStringLiteral(".sse"),
    };
    const QString suffix = QFileInfo(full_path).suffix().toLower();
    if (suffix.isEmpty()) return false;
    const QString dot = QLatin1Char('.') + suffix;
    return kExtensions.contains(dot);
}

}  // namespace ui
