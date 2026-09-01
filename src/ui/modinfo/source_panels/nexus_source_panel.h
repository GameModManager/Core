#pragma once

#include "ui/modinfo/source_panels/source_info_panel.h"

#include "engine/source/nexus_provider.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>

namespace ui {

class SourceFetchThread;

// Full Nexus Source panel - ported from SourceTab::build_nexus_page.
// Layout matches the original exactly: QForm rows Mod ID / Source game /
// Version / Category, button row Refresh + Visit + optional Endorse/Track,
// Custom URL row, QTextBrowser description.
class NexusSourcePanel : public SourceInfoPanel {
  Q_OBJECT
public:
  explicit NexusSourcePanel(const ModInfoData &data, QWidget *parent = nullptr);
  ~NexusSourcePanel() override;

  void populate() override;
  void save_state() override;
  [[nodiscard]] bool has_data() const override;

private:
  void update_version_color();
  void render_description();
  void on_refresh();
  void launch_fetch();
  void on_fetch_finished(engine::ModInfoResult result, quint64 generation);
  void apply_fetch_result(const engine::ModInfoResult &result);
  void on_visit();
  void on_visit_custom();
  void on_custom_url_toggled();
  void persist_fields();
  void persist_custom_url();

  QLineEdit *mod_id_ = nullptr;
  QComboBox *source_game_ = nullptr;
  QLineEdit *version_ = nullptr;
  QLineEdit *category_ = nullptr;
  QPushButton *refresh_ = nullptr;
  QPushButton *visit_ = nullptr;
  QCheckBox *custom_url_toggle_ = nullptr;
  QLineEdit *custom_url_ = nullptr;
  QPushButton *visit_custom_ = nullptr;
  QTextBrowser *description_ = nullptr;

  SourceFetchThread *source_fetch_thread_ = nullptr;
  quint64 refresh_generation_ = 0;
  QString refresh_mod_id_;
  bool fetch_in_flight_ = false;
  bool refresh_pending_ = false;
};

} // namespace ui
