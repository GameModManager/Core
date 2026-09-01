#pragma once

#include "engine/mod/meta/mod_meta.h"
#include "ui/modinfo/mod_info_data.h"

#include <QWidget>

namespace ui {

// Abstract base for all Source Info sub-panels (Nexus, Steam, generic).
// Holds the ModInfoData for the current mod and provides shared helpers
// for reading/writing ModMeta through the data's load_meta/save_meta
// lambdas. Subclasses own their widgets and implement populate/save_state.
class SourceInfoPanel : public QWidget {
  Q_OBJECT
public:
  explicit SourceInfoPanel(const ModInfoData &data, QWidget *parent = nullptr)
      : QWidget(parent), data_(data) {}
  ~SourceInfoPanel() override;

  const ModInfoData &data() const { return data_; }

  // Fill widgets from data_/ModMeta. Called after construction and on
  // every refresh/populate.
  virtual void populate() = 0;

  // Persist any pending edits to ModMeta. Called by SourceTab::save_state().
  virtual void save_state() {}

  // Whether this panel has meaningful data for has_data aggregation.
  [[nodiscard]] virtual bool has_data() const { return false; }

protected:
  QString meta_value(const char *section, const char *key) const {
    if (!data_.load_meta)
      return {};
    return QString::fromStdString(data_.load_meta().get(section, key));
  }

  void set_meta_value(const char *section, const char *key, const QString &v) {
    if (!data_.load_meta || !data_.save_meta)
      return;
    auto meta = data_.load_meta();
    const QString before = QString::fromStdString(meta.get(section, key));
    if (before == v)
      return;
    meta.set(section, key, v.toStdString());
    data_.save_meta(meta);
  }

  ModInfoData data_;
  bool loading_ = false;
};

} // namespace ui
