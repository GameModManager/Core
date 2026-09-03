#pragma once

#include "ui/modinfo/source_panels/source_info_panel.h"

#include "ui/modinfo/loverslab_fetch_worker.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <atomic>

namespace ui {

class DescriptionBrowser;

// LoversLab Source panel - mirrors the Nexus layout but omits the Source
// game row and the Endorse/Track buttons. Refresh runs the LoversLab
// Provider::fetch_mod_info scrape on the worker thread and writes the
// result into the mod's [LoversLab] meta section. Also exposes the
// dateModified/installed comparison (out-of-date badge) the user asked
// for: when the page's dateModified falls after the mod's install time
// the panel renders a red "Out of date" label.
class LoversLabSourcePanel : public SourceInfoPanel {
  Q_OBJECT
public:
  explicit LoversLabSourcePanel(const ModInfoData &data,
                                QWidget *parent = nullptr);
  ~LoversLabSourcePanel() override;

  void populate() override;
  void save_state() override;
  [[nodiscard]] bool has_data() const override;

private:
  // Date comparison: parses the page's dateModified (ISO 8601, possibly
  // date-only) against the mod's installation timestamp (epoch seconds),
  // normalized to date granularity so a mod downloaded on the same day
  // does not show as out-of-date just because the page stamp is later in
  // the day. Returns -1 (page earlier / not set), 0 (same day / unknown),
  // +1 (page strictly later = out of date).
  enum class DateCompare {
    Unknown,
    PageOlder,
    SameDay,
    PageNewer,
  };
  DateCompare compare_dates() const;
  void update_out_of_date_label();

  void update_version_color();
  void render_description();
  void on_refresh();
  void launch_fetch();
  void on_fetch_finished(engine::LoversLabModInfoResult result,
                         quint64 generation);
  void apply_fetch_result(const engine::LoversLabModInfoResult &result);
  void on_visit();
  void on_visit_custom();
  void on_custom_url_toggled();
  void persist_fields();
  void persist_custom_url();

  QLineEdit *mod_id_ = nullptr;
  QLineEdit *version_ = nullptr;
  QLineEdit *category_ = nullptr;
  QLabel *out_of_date_label_ = nullptr;
  QPushButton *refresh_ = nullptr;
  QPushButton *visit_ = nullptr;
  QCheckBox *custom_url_toggle_ = nullptr;
  QLineEdit *custom_url_ = nullptr;
  QPushButton *visit_custom_ = nullptr;
  DescriptionBrowser *description_ = nullptr;

  LoversLabFetchThread *source_fetch_thread_ = nullptr;
  quint64 refresh_generation_ = 0;
  QString refresh_mod_id_;
  bool fetch_in_flight_ = false;
  bool refresh_pending_ = false;

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