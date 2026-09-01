#pragma once

#include "ui/modinfo/source_panels/source_info_panel.h"

namespace engine::Source {
class Interface;
}

namespace ui {

class GenericSourcePanel : public SourceInfoPanel {
  Q_OBJECT
public:
  GenericSourcePanel(const ModInfoData &data,
                     engine::Source::Interface *provider,
                     QWidget *parent = nullptr);

  void populate() override;
  [[nodiscard]] bool has_data() const override;

private:
  // Retained for future provider-specific actions; consumed in ctor to build
  // labels. populate() is intentionally no-op because SourceTab recreates
  // panels on every populate().
  engine::Source::Interface *provider_ = nullptr;
};

} // namespace ui
