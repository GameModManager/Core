#include "ui/modinfo/images_tab.h"

#include "ui/viewer/file_viewer.h"
#include "ui/viewer/image_viewer.h"

#include <QDesktopServices>
#include <QDirIterator>
#include <QDialog>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

namespace {

QStringList supported_image_suffixes() {
    QStringList suffixes;
    const auto formats = QImageReader::supportedImageFormats();
    for (const auto& f : formats) suffixes << QLatin1Char('.') + f.toLower();
    if (!suffixes.contains(QStringLiteral(".dds"))) suffixes << QStringLiteral(".dds");
    return suffixes;
}

// Full-size preview window (double-click on a thumbnail).
class FullSizePreview : public QDialog {
public:
    explicit FullSizePreview(const QString& path, QWidget* parent = nullptr)
        : QDialog(parent), viewer_(new FileViewer(this)) {
        setWindowTitle(QFileInfo(path).fileName());
        resize(800, 600);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(viewer_);
        viewer_->open(path);
    }

private:
    FileViewer* viewer_ = nullptr;
};

}  // namespace

ImagesTab::ImagesTab(QWidget* parent) : ModInfoTab(parent) {
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);

    auto* left = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    filter_ = new QLineEdit(left);
    filter_->setPlaceholderText(tr("Filter..."));
    left_layout->addWidget(filter_);

    auto* explore_btn = new QPushButton(tr("Explore"), left);
    left_layout->addWidget(explore_btn);

    thumbnails_ = new QListWidget(left);
    thumbnails_->setViewMode(QListView::IconMode);
    thumbnails_->setIconSize(QSize(96, 96));
    thumbnails_->setResizeMode(QListView::Adjust);
    thumbnails_->setMovement(QListView::Static);
    thumbnails_->setUniformItemSizes(true);
    thumbnails_->setTextElideMode(Qt::ElideMiddle);
    left_layout->addWidget(thumbnails_, 1);

    preview_ = new FileViewer(this);
    splitter_->addWidget(left);
    splitter_->addWidget(preview_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({170, 1});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter_);

    connect(filter_, &QLineEdit::textChanged, this, &ImagesTab::apply_filter);
    connect(thumbnails_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                select_image(current ? current->data(Qt::UserRole).toString()
                                     : QString());
            });
    connect(thumbnails_, &QListWidget::itemDoubleClicked, this,
            &ImagesTab::open_full_size);
    connect(explore_btn, &QPushButton::clicked, this, &ImagesTab::open_explorer);
}

ImagesTab::~ImagesTab() = default;

void ImagesTab::set_mod(const ModInfoData& data) {
    files_.clear();
    icon_cache_.clear();
    current_path_.clear();
    preview_->clear();

    // The mod folder root IS the game-data root (MO2 layout); data_dir()
    // appends mods_subpath ("Data") which mods never contain.
    const QDir root = data.mod_dir;
    if (root.exists()) {
        const auto suffixes = supported_image_suffixes();
        QDirIterator it(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString full = it.next();
            const QString suffix = QFileInfo(full).suffix().toLower();
            if (!suffix.isEmpty() &&
                suffixes.contains(QLatin1Char('.') + suffix)) {
                files_.push_back({full, root.relativeFilePath(full)});
            }
        }
    }

    std::sort(files_.begin(), files_.end(),
              [](const ImageFile& a, const ImageFile& b) {
                  return a.text < b.text;
              });
    set_has_data(!files_.empty());
    rebuild_list();
    apply_filter();
}

void ImagesTab::first_activation() {
    for (int i = 0; i < thumbnails_->count(); ++i) {
        auto* item = thumbnails_->item(i);
        const QString path = item->data(Qt::UserRole).toString();
        if (icon_cache_.contains(path)) continue;
        QImageReader reader(path);
        const QImage img = reader.read();
        if (img.isNull()) continue;
        icon_cache_[path] = QIcon(QPixmap::fromImage(img.scaled(
            96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        item->setIcon(icon_cache_[path]);
    }
}

void ImagesTab::rebuild_list() {
    thumbnails_->clear();
    for (const auto& f : files_) {
        auto* item = new QListWidgetItem(f.text, thumbnails_);
        item->setData(Qt::UserRole, f.path);
        item->setToolTip(f.text);
        item->setSizeHint(QSize(110, 96 + thumbnails_->fontMetrics().height()));
    }
    thumbnails_->setEnabled(!files_.empty());
}

void ImagesTab::apply_filter() {
    const QString needle = filter_->text().trimmed();
    for (int row = 0; row < thumbnails_->count(); ++row) {
        auto* item = thumbnails_->item(row);
        const bool visible =
            needle.isEmpty() ||
            item->text().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
    }
}

void ImagesTab::select_image(const QString& path) {
    if (path == current_path_) return;
    if (!maybe_flush_preview()) return;
    current_path_ = path;
    if (path.isEmpty()) {
        preview_->clear();
        return;
    }
    preview_->open(path);
}

void ImagesTab::open_full_size() {
    auto* current = thumbnails_->currentItem();
    if (!current) return;
    const QString path = current->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;
    if (!maybe_flush_preview()) return;
    FullSizePreview dlg(path, this);
    dlg.exec();
    preview_->open(path);  // re-attach the file after the dialog steals it
}

void ImagesTab::open_explorer() {
    if (current_path_.isEmpty()) return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(current_path_).absolutePath()));
}

bool ImagesTab::maybe_flush_preview() {
    // Preview selection carries no editable state (MO2 warns about its preview
    // only for plugins); nothing to flush.
    return true;
}

void ImagesTab::save_state() {}

bool ImagesTab::can_close() {
    return true;
}

}  // namespace ui
