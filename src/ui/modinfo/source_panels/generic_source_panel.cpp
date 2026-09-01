#include "ui/modinfo/source_panels/generic_source_panel.h"

#include "engine/source/interface.h"

#include <QFormLayout>
#include <QLabel>

namespace ui {

GenericSourcePanel::GenericSourcePanel(const ModInfoData &data,
                                       engine::Source::Interface *provider,
                                       QWidget *parent)
    : SourceInfoPanel(data, parent), provider_(provider) {
  auto *form = new QFormLayout(this);
  form->setContentsMargins(0, 0, 0, 0);

  auto *source_name =
      new QLabel(QString::fromStdString(provider_->display_name()), this);
  source_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(tr("Source:"), source_name);

  auto *id_label = new QLabel(data_.source_id.isEmpty()
                                  ? tr("(not installed from this source)")
                                  : data_.source_id,
                              this);
  id_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(tr("Source ID:"), id_label);
}

void GenericSourcePanel::populate() {}

bool GenericSourcePanel::has_data() const { return !data_.source_id.isEmpty(); }

} // namespace ui
