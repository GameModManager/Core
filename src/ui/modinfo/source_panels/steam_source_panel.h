#pragma once

#include "ui/modinfo/source_panels/source_info_panel.h"

#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>

#include <atomic>

namespace ui {

class DescriptionBrowser;

// Steam Workshop Source panel. Mirrors the Nexus layout but omits
// the Source Game row and the Endorse/Track buttons. Fields are bound
// to Steam Workshop keys and metadata.xml where applicable.
class SteamSourcePanel : public SourceInfoPanel {
  Q_OBJECT
public:
  explicit SteamSourcePanel(const ModInfoData &data, QWidget *parent = nullptr);
  ~SteamSourcePanel() override = default;

  void populate() override;
  void save_state() override;
  [[nodiscard]] bool has_data() const override;

private:
  QString read_metadata_tag(const QString &tag) const;
  void update_version_color();
  void render_description();
  void on_refresh();
  void on_visit();
  void on_visit_custom();
  void on_custom_url_toggled();
  void persist_fields();
  void persist_custom_url();

  QLineEdit *mod_id_ = nullptr;
  QLineEdit *version_ = nullptr;
  QLineEdit *category_ = nullptr;
  QPushButton *refresh_ = nullptr;
  QPushButton *visit_ = nullptr;
  QCheckBox *custom_url_toggle_ = nullptr;
  QLineEdit *custom_url_ = nullptr;
  QPushButton *visit_custom_ = nullptr;
  DescriptionBrowser *description_ = nullptr;

  // Monotonic counter incremented every time render_description() dispatches
  // a BBCode parse to the thread pool. The async callback compares against
  // the current value to drop stale results when the user clicks rapidly
  // through the mod list and an older parse finishes after a newer one.
  // DECLARATION ORDER: must be after description_ so the atomic is destroyed
  // before the browser during panel teardown. The async lambda captures a raw
  // pointer to this atomic; the QPointer guard prevents reaching the
  // dereference.
  std::atomic<unsigned> description_generation_{0};
};

} // namespace ui
