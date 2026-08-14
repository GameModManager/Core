#include "ui/modinfo/notes_tab.h"

#include "engine/mod/meta/mod_meta.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QStyle>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ui {

namespace {
QString ideal_text_color(const QColor& bg) {
    const qreal luminance =
        0.299 * bg.red() + 0.587 * bg.green() + 0.114 * bg.blue();
    return luminance > 150 ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
}
}  // namespace

NotesTab::NotesTab(QWidget* parent) : ModInfoTab(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    comments_ = new QLineEdit(this);
    comments_->setPlaceholderText(tr("Comments..."));
    layout->addWidget(comments_);

    notes_ = new QTextEdit(this);
    layout->addWidget(notes_, 1);

    auto* color_row = new QHBoxLayout();
    set_color_ = new QPushButton(tr("Set color..."), this);
    reset_color_ = new QPushButton(tr("Reset color"), this);
    color_row->addWidget(set_color_);
    color_row->addWidget(reset_color_);
    color_row->addStretch(1);
    layout->addLayout(color_row);

    connect(comments_, &QLineEdit::editingFinished, this,
            &NotesTab::on_comments_edited);
    connect(notes_, &QTextEdit::textChanged, this, [this]() {
        notes_dirty_ = true;
    });
    connect(set_color_, &QPushButton::clicked, this, &NotesTab::on_set_color);
    connect(reset_color_, &QPushButton::clicked, this, &NotesTab::on_reset_color);
}

NotesTab::~NotesTab() = default;

void NotesTab::set_mod(const ModInfoData& data) {
    const auto meta = data.load_meta();
    comments_->setText(
        QString::fromStdString(meta.get("General", "comments")));

    const QString notes = QString::fromStdString(meta.get("General", "notes"));
    if (notes.isEmpty())
        notes_->clear();
    else
        notes_->setHtml(notes);
    notes_dirty_ = false;

    // Color editing is a separator-only feature (the only mod type GMM colors).
    set_color_->setVisible(data.is_separator);
    reset_color_->setVisible(data.is_separator);

    update_comments_color();
    set_has_data(!comments_->text().isEmpty() ||
                 !notes_->toPlainText().isEmpty() ||
                 data.color_value().isValid());
}

void NotesTab::update_comments_color() {
    const QColor color = current().color_value();
    QPalette pal = comments_->palette();
    if (color.isValid()) {
        pal.setColor(QPalette::Base, color);
        pal.setColor(QPalette::Text, QColor(ideal_text_color(color)));
    } else {
        pal = style()->standardPalette();
    }
    comments_->setPalette(pal);
}

void NotesTab::on_comments_edited() {
    auto meta = current().load_meta();
    const QString text = comments_->text();
    const QString before = QString::fromStdString(meta.get("General", "comments"));
    if (before != text) {
        meta.set("General", "comments", text.toStdString());
        current().save_meta(meta);
    }
}

void NotesTab::persist_notes() {
    if (!notes_dirty_) return;
    auto meta = current().load_meta();
    if (notes_->toPlainText().isEmpty()) {
        // Avoid persisting the HTML wrapper for an empty note (MO2 parity).
        meta.set("General", "notes", "");
    } else {
        meta.set("General", "notes", notes_->toHtml().toStdString());
    }
    current().save_meta(meta);
    notes_dirty_ = false;
}

void NotesTab::on_set_color() {
    if (!current().set_mod_color) return;
    QColorDialog dialog(this);
    dialog.setOption(QColorDialog::ShowAlphaChannel);
    const QColor current_color = current().color_value();
    if (current_color.isValid()) dialog.setCurrentColor(current_color);
    if (dialog.exec() != QDialog::Accepted) return;
    const QColor color = dialog.currentColor();
    if (!color.isValid()) return;

    auto meta = current().load_meta();
    meta.set("General", "color", color.name(QColor::HexArgb).toStdString());
    current().save_meta(meta);
    current().set_mod_color(color);
    update_comments_color();
}

void NotesTab::on_reset_color() {
    if (!current().set_mod_color) return;
    auto meta = current().load_meta();
    const bool had = !meta.get("General", "color").empty();
    if (had) {
        // Rebuild without the color key (ModMeta has no remove).
        engine::ModMeta rebuilt;
        for (const auto& section : meta.sections()) {
            for (const auto& key : meta.keys(section)) {
                if (section == "General" && key == "color") continue;
                rebuilt.set(section, key, meta.get(section, key));
            }
        }
        current().save_meta(rebuilt);
    }
    current().set_mod_color(QColor());
    update_comments_color();
}

void NotesTab::save_state() {
    on_comments_edited();
    persist_notes();
}

}  // namespace ui
