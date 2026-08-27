#include "ui/preview/preview_window.h"
#include "ui/preview/preview_widget.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>

namespace ui::preview {

namespace {

bool has_extension(const QString& path, const QStringList& exts) {
    return exts.contains(QFileInfo(path).suffix().toLower());
}

const QStringList& image_extensions() {
    static const QStringList exts = {"png", "jpg", "jpeg", "webp", "bmp", "gif", "anm2"};
    return exts;
}

const QStringList& text_extensions() {
    static const QStringList exts = {"txt", "ini", "cfg", "log", "json", "xml",
                                     "meta", "md"};
    return exts;
}

}  // namespace

bool PreviewWindow::supports(const QString& file_path) {
    return has_extension(file_path, image_extensions()) ||
           has_extension(file_path, text_extensions());
}

PreviewWindow::PreviewWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Preview"));
    setMinimumSize(440, 380);
    resize(680, 520);

    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    name_label_ = new QLabel(this);
    name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(name_label_, 1);
    prev_button_ = new QPushButton(tr("Previous"), this);
    next_button_ = new QPushButton(tr("Next"), this);
    top->addWidget(prev_button_);
    top->addWidget(next_button_);
    layout->addLayout(top);

    source_label_ = new QLabel(this);
    source_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    source_label_->setEnabled(false);
    layout->addWidget(source_label_);

    stack_ = new QStackedWidget(this);

    // Image page: scrollable image on a checkerboard background + zoom bar.
    image_page_ = new QWidget(this);
    auto* image_layout = new QVBoxLayout(image_page_);
    image_layout->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(image_page_);
    scroll_->setWidgetResizable(true);
    scroll_->setAlignment(Qt::AlignCenter);
    image_label_ = new QLabel(scroll_);
    image_label_->setAlignment(Qt::AlignCenter);
    auto pal = image_label_->palette();
    pal.setBrush(QPalette::Base, QBrush(checker_pixmap("auto")));
    pal.setBrush(QPalette::Window, QBrush(checker_pixmap("auto")));
    image_label_->setPalette(pal);
    scroll_->viewport()->setPalette(pal);
    scroll_->setWidget(image_label_);
    image_layout->addWidget(scroll_);

    auto* zoom_bar = new QHBoxLayout;
    auto* fit_button = new QPushButton(tr("Fit"), image_page_);
    auto* zoom_out_button = new QPushButton("-", image_page_);
    auto* zoom_in_button = new QPushButton("+", image_page_);
    zoom_label_ = new QLabel("100%", image_page_);
    zoom_bar->addWidget(fit_button);
    zoom_bar->addWidget(zoom_out_button);
    zoom_bar->addWidget(zoom_in_button);
    zoom_bar->addStretch(1);
    zoom_bar->addWidget(zoom_label_);
    image_layout->addLayout(zoom_bar);

    stack_->addWidget(image_page_);

    // Text page: read-only monospace view.
    text_view_ = new QTextBrowser(this);
    text_view_->setReadOnly(true);
    QFont mono = text_view_->font();
    mono.setFamily(QStringLiteral("monospace"));
    text_view_->setFont(mono);
    stack_->addWidget(text_view_);

    // Unsupported page.
    unsupported_label_ = new QLabel(tr("No preview available for this file type."), this);
    unsupported_label_->setAlignment(Qt::AlignCenter);
    unsupported_label_->setEnabled(false);
    stack_->addWidget(unsupported_label_);

    layout->addWidget(stack_, 1);

    connect(fit_button, &QPushButton::clicked, this, &PreviewWindow::set_fit);
    connect(zoom_in_button, &QPushButton::clicked, this, [this]() { zoom_by(1.25); });
    connect(zoom_out_button, &QPushButton::clicked, this, [this]() { zoom_by(0.8); });
    connect(prev_button_, &QPushButton::clicked, this, [this]() {
        if (variant_ > 0) --variant_;
        reload();
    });
    connect(next_button_, &QPushButton::clicked, this, [this]() {
        if (variant_ + 1 < paths_.size()) ++variant_;
        reload();
    });
}

void PreviewWindow::show_file(const QString& file_path,
                              const QStringList& provider_paths,
                              const QStringList& provider_names) {
    paths_.clear();
    names_.clear();
    paths_ << file_path;
    names_ << QString();

    // Append any provider variants (skipping the primary, already first, and
    // entries without a resolvable on-disk copy).
    for (int i = 0; i < provider_paths.size(); ++i) {
        const auto& p = provider_paths[i];
        if (p.isEmpty() || p == file_path) continue;
        paths_ << p;
        names_ << (i < provider_names.size() ? provider_names[i] : QString());
    }

    variant_ = 0;
    zoom_ = 1.0;
    fit_ = true;
    reload();
    show();
    raise();
    activateWindow();
}

void PreviewWindow::reload() {
    const int count = static_cast<int>(paths_.size());
    prev_button_->setEnabled(variant_ > 0);
    next_button_->setEnabled(variant_ + 1 < count);

    const QString& path = paths_[variant_];
    name_label_->setText(QFileInfo(path).fileName());
    if (variant_ > 0 && variant_ < names_.size() && !names_[variant_].isEmpty()) {
        source_label_->setText(tr("Variant %1/%2 - %3")
                                   .arg(variant_ + 1).arg(count).arg(names_[variant_]));
        source_label_->setEnabled(true);
    } else if (count > 1) {
        source_label_->setText(tr("Variant %1/%2").arg(variant_ + 1).arg(count));
        source_label_->setEnabled(true);
    } else {
        source_label_->clear();
        source_label_->setEnabled(false);
    }

    zoom_label_->setText(QStringLiteral("100%"));
    if (load_image(path) || load_text(path))
        return;
    show_unsupported();
}

bool PreviewWindow::load_image(const QString& path) {
    if (!has_extension(path, image_extensions())) return false;
    QPixmap pm(path);
    if (pm.isNull()) return false;
    current_pixmap_ = pm;
    stack_->setCurrentWidget(image_page_);
    apply_zoom();
    return true;
}

bool PreviewWindow::load_text(const QString& path) {
    if (!has_extension(path, text_extensions())) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    // Cap at 8 MiB so a stray huge log can't stall the UI.
    const qint64 limit = 8 * 1024 * 1024;
    QByteArray data = file.read(limit + 1);
    if (data.size() > limit)
        data = data.left(limit);
    text_view_->setPlainText(QString::fromUtf8(data));
    stack_->setCurrentWidget(text_view_);
    return true;
}

void PreviewWindow::show_unsupported() {
    current_pixmap_ = QPixmap();
    stack_->setCurrentWidget(unsupported_label_);
}

void PreviewWindow::set_fit() {
    fit_ = true;
    zoom_ = 1.0;
    apply_zoom();
}

void PreviewWindow::zoom_by(double factor) {
    if (current_pixmap_.isNull()) return;
    if (fit_) {
        // Leave fit mode from natural size.
        fit_ = false;
        zoom_ = 1.0;
    }
    zoom_ *= factor;
    zoom_ = std::clamp(zoom_, 0.05, 64.0);
    apply_zoom();
}

void PreviewWindow::apply_zoom() {
    if (current_pixmap_.isNull()) {
        image_label_->clear();
        return;
    }
    QSize target;
    if (fit_) {
        const auto vp = scroll_->viewport()->size();
        if (vp.isEmpty()) {
            image_label_->setPixmap(current_pixmap_);
            return;
        }
        target = vp;
    } else {
        target = current_pixmap_.size() * zoom_;
    }
    target = target.expandedTo(QSize(1, 1));
    image_label_->setPixmap(current_pixmap_.scaled(
        target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (!fit_)
        zoom_label_->setText(QString::number(qRound(zoom_ * 100.0)) + "%");
    else
        zoom_label_->setText(QStringLiteral("100%"));
}

void PreviewWindow::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (fit_ && stack_ && stack_->currentWidget() == image_page_)
        apply_zoom();
}

}  // namespace ui::preview
