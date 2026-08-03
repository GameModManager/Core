#include "ui/widgets/mod_info_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

namespace ui {

ModInfoDialog::ModInfoDialog(const Data& data, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Mod Info"));
    setMinimumWidth(360);

    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(data.name.isEmpty() ? data.folder : data.name, this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title_font.setPointSize(title_font.pointSize() + 2);
    title->setFont(title_font);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    add_row(form, tr("Folder"), data.folder);
    add_row(form, tr("State"), data.enabled ? tr("Enabled") : tr("Disabled"));
    add_row(form, tr("Priority"), QString::number(data.priority));
    if (!data.version.isEmpty())
        add_row(form, tr("Version"), data.version);
    if (!data.source.isEmpty())
        add_row(form, tr("Source"), data.source);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    auto* stats = new QFormLayout;
    add_row(stats, tr("Files"), QString::number(data.file_count));
    add_row(stats, tr("Conflicts won"), QString::number(data.conflict_wins));
    add_row(stats, tr("Conflicts lost"), QString::number(data.conflict_losses));
    layout->addLayout(stats);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void ModInfoDialog::add_row(QFormLayout* form, const QString& label, const QString& value) {
    auto* value_label = new QLabel(value, parentWidget());
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(label, value_label);
}

}  // namespace ui
