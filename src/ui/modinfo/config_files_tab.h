#pragma once

#include "ui/modinfo/generic_files_tab.h"

namespace ui {

// Config files inside the mod folder root: .ini/.cfg/.toml/.yaml/.yml/.json,
// except the mod's own meta.ini (that belongs to the manager, not the game).
// The extension list is a single static set - extend it to cover more formats.
class ConfigFilesTab : public GenericFilesTab {
    Q_OBJECT
public:
    explicit ConfigFilesTab(QWidget* parent = nullptr);

protected:
    bool wants_file(const QString& rel_path,
                    const QString& full_path) const override;
};

}  // namespace ui
