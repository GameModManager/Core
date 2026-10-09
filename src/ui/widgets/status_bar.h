#pragma once

#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QWidget>
#include <QStringList>
#include <QTimer>

class QFrame;
class QProgressBar;
class QToolButton;

namespace ui {

// The resting text at the far left of the status bar: "<game> - <instance> -
// <profile>", each component replaced by a fallback when that value is
// genuinely absent. Free function so the composition is testable without a
// MainWindow.
QString context_label_text(const QString &game, const QString &instance,
                           const QString &profile);

class StatusBar : public QWidget {
  Q_OBJECT
public:
  explicit StatusBar(QWidget *parent = nullptr);

  // Set the resting left-hand text. Takes effect at once unless a transient
  // status is on screen, which owns the left-hand text until it times out.
  void set_context(const QString &text);
  // Show a transient message. Restores the context text when it expires.
  void set_status(const QString &text);

  // Determinacy bar for a multi-step operation (MO2 StatusBar::setProgress,
  // statusbar.cpp:58-70). `percent` outside 0..99 hides the bar and gives the
  // left-hand label back to the context text; inside it, the bar shows the
  // percentage and the label reads "Loading...".
  void set_progress(int percent);
  // 0-100 when the bar is showing, -1 when it is hidden.
  [[nodiscard]] int progress() const;

  // Configure what the status bar shows for the current game. Only sources
  // that meter a request budget get a label - see SourceRateLimit.
  void set_sources(const QStringList &sources);  // e.g. {"Nexus Mods", "Steam"}

  QString context_text() const { return context_; }
  // The sources that have a readout, and what each currently reads.
  QStringList source_names() const;
  QString source_text(const QString &name) const;

signals:
  void pipeline_clicked();

private:
  void refresh_pipeline_indicator();
  // Re-read every metered source's budget into its label.
  void refresh_source_meters();
  void clear_source_labels();

  QHBoxLayout *layout_  = nullptr;
  QLabel *status_label_ = nullptr;
  // The resting left-hand text; the single source of truth for what the left
  // label shows when no transient message is on screen.
  QString context_;
  QList<QLabel *> source_labels_;
  QMap<QString, QLabel *> source_labels_by_name_;
  QFrame *separator_            = nullptr;
  QToolButton *pipeline_button_ = nullptr;
  // MO2 statusbar.cpp:31-37: 0-100, percent visible, capped at 300px wide so
  // it cannot crowd the meters beside it. Hidden until set_progress() is
  // called, so it never advertises work that is not running.
  QProgressBar *progress_ = nullptr;
  QTimer *pipeline_timer_ = nullptr;
  QTimer *status_timer_   = nullptr;
};

}  // namespace ui
