#pragma once

#include "ui/modinfo/source_panels/source_info_panel.h"

#include "ui/modinfo/modpub_fetch_worker.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <atomic>

namespace ui {

class DescriptionBrowser;

// ModPub Source panel - mirrors the LoversLab layout (Refresh + Visit +
// description) but omits the out-of-date badge (mod.pub's dateModified
// is inconsistently populated across mod pages - the JSON-LD field is
// always present but not always meaningful, and the badge was a LoverLab-
// specific feature). Refresh runs the ModPub Provider::fetch_mod_info
// scrape on the worker thread and writes the result into the mod's
// [ModPub] meta section.
//
// ModPub is metadata-only: downloads go through the companion modl://
// protocol, so the panel never initiates a download. The Visit button
// opens the page URL the user originally pasted (or the bare
// https://mod.pub/<id>/ fallback).
class ModPubSourcePanel : public SourceInfoPanel {
  Q_OBJECT
public:
  explicit ModPubSourcePanel(const ModInfoData &data,
                             QWidget *parent = nullptr);
  ~ModPubSourcePanel() override;

  void populate() override;
  void save_state() override;
  [[nodiscard]] bool has_data() const override;

private:
  void render_description();
  void on_refresh();
  void launch_fetch();
  void on_fetch_finished(engine::ModPubModInfoResult result,
                         quint64 generation);
  void apply_fetch_result(const engine::ModPubModInfoResult &result);
  void on_visit();
  void persist_fields();

  QLineEdit *mod_id_ = nullptr;
  QLineEdit *version_ = nullptr;
  QLineEdit *category_ = nullptr;
  QLineEdit *author_ = nullptr;
  QLineEdit *page_url_ = nullptr;
  QPushButton *refresh_ = nullptr;
  QPushButton *visit_ = nullptr;
  DescriptionBrowser *description_ = nullptr;

  ModPubFetchThread *source_fetch_thread_ = nullptr;
  quint64 refresh_generation_ = 0;
  QString refresh_mod_id_;
  bool fetch_in_flight_ = false;
  bool refresh_pending_ = false;

  // Monotonic counter for the async BBCode parse. Increments every time
  // render_description() dispatches; the worker compares to drop stale
  // results when the user clicks rapidly through the mod list.
  // DECLARATION ORDER: must be after description_ so the atomic is
  // destroyed before the browser during panel teardown.
  std::atomic<unsigned> description_generation_{0};
};

} // namespace ui
