#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QIcon>

#include <map>
#include <vector>

class QLineEdit;
class QListWidget;
class QSplitter;

namespace ui {

class FileViewer;

// MO2's ImagesTab: a filterable thumbnail strip on the left, an inline preview
// (the reusable FileViewer) on the right. Double-click opens the file
// full-size in its own window. "Explore" reveals the images folder.
class ImagesTab : public ModInfoTab {
    Q_OBJECT
public:
    explicit ImagesTab(QWidget* parent = nullptr);
    ~ImagesTab() override;

    void set_mod(const ModInfoData& data) override;
    void first_activation() override;
    void save_state() override;
    bool can_close() override;

private:
    struct ImageFile {
        QString path;
        QString text;
    };

    void rebuild_list();
    void apply_filter();
    void select_image(const QString& path);
    void open_full_size();
    void open_explorer();
    bool maybe_flush_preview();

    QSplitter* splitter_ = nullptr;
    QListWidget* thumbnails_ = nullptr;
    QLineEdit* filter_ = nullptr;
    FileViewer* preview_ = nullptr;
    std::vector<ImageFile> files_;
    std::map<QString, QIcon> icon_cache_;
    QString current_path_;
};

}  // namespace ui
