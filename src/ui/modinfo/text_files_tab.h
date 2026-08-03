#pragma once

#include "ui/modinfo/generic_files_tab.h"

namespace ui {

// MO2's TextFilesTab: any human-readable text file inside the mod's Data dir.
class TextFilesTab : public GenericFilesTab {
    Q_OBJECT
public:
    explicit TextFilesTab(QWidget* parent = nullptr);

protected:
    bool wants_file(const QString& rel_path,
                    const QString& full_path) const override;
};

}  // namespace ui
