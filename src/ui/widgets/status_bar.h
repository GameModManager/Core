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

  // Persistent notice about Nexus mod updates, on the left of the source
  // meters. Unlike set_status() this does NOT time out: "12 mods have
  // updates" has to survive until the user deals with it, which is the whole
  // point of reporting it - a message that vanished after five seconds is the
  // "hunting" this exists to avoid.
  //
  // `warning` tints the text, and is what distinguishes "the check could not
  // be made" from "the check says there is nothing". Callers must pass
  // warning=true whenever the count could not be established, so a refused
  // check never reads as an up-to-date library.
  void set_update_notice(const QString &text, bool warning = false);
  [[nodiscard]] QString update_notice() const { return update_notice_; }
  [[nodiscard]] bool update_notice_is_warning() const { return update_warning_; }

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
  QLabel *update_label_ = nullptr;
  // The update notice as last set, kept so a caller can read back what the
  // bar is claiming and a theme change can re-apply the tint.
  QString update_notice_;
  bool update_warning_ = false;
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
