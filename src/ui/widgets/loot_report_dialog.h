#pragma once

#include "engine/sort/sorter/loot/sorter.h"

#include <QDialog>
#include <QString>
#include <vector>

class QLabel;
class QPushButton;
class QTextEdit;

namespace ui {

// The LOOT report popup: MO2's LootDialog report pane, minus the parts GMM
// cannot honestly claim.
//
// What MO2 does, read from references/modorganizer/src/:
//   - loot.cpp:960-984 runLoot(): a LootDialog opens the moment the sort
//     starts and stays up until the user closes it; showReport() fills the
//     report pane once, on finish. So the pane appears WHILE the sort runs
//     but the report is a SUMMARY written at the end, not a stream.
//   - lootdialog.cpp:93-94 + 221-223 m_report.setText("Running LOOT..."):
//     the pane carries placeholder text for the whole run.
//   - lootdialog.cpp:96-107 setProgress(): the progress line advances per
//     stage while the run lasts.
//   - lootdialog.cpp:287-300 showReport(): sorted plugin list markdown, then
//     lootReport.toMarkdown().
//   - loot.cpp:240-274 successMarkdown(): the game-wide messages come FIRST,
//     under "### General messages", ahead of the per-plugin sections - that
//     is the "shown above the plugin list" placement.
//   - loot.cpp:391-412 Message::toMarkdown(): "**Error**: " / "**Warning**: "
//     prefixes, info messages bare.
//   - loot.cpp:303-308 Stats::toMarkdown(): the trailing stats line.
//
// What is deliberately NOT here: the QWebEngineView + markdown.html viewer
// and the sorted-plugin-list markdown. Rendering markdown through a web view
// is its own featuremap row ("LOOT report viewer (markdown + web)") and stays
// unimplemented rather than half-claimed. This pane renders the general
// messages through Qt's own markdown-in-QTextEdit, which needs no web engine.
//
// Modal vs modeless: MO2 runs its dialog modally and disables the top-level
// window (pluginlistview.cpp:201-203). Ours is modeless. The sort runs on a
// worker thread with queued signals, so a modal dialog here would only block
// the rest of the app for no gain; what the user asked for - the popup opens
// as the sort runs, the report lands once at the end - is preserved exactly.
class LootReportDialog : public QDialog {
  Q_OBJECT
public:
  explicit LootReportDialog(QWidget *parent = nullptr);

  // Progress line while the sort runs. MO2 LootDialog::setProgress
  // (lootdialog.cpp:96-107), which writes the same stage names.
  void set_stage(const QString &stage);

  // The finished report: "General messages" plus the stats line. Replaces
  // whatever the pane showed. Call once, when the run ends.
  void show_report(const std::vector<engine::Sorter::Loot::Result::Message> &messages,
                   const engine::Sorter::Loot::Stats &stats);

  // The run ended without a new order - LOOT itself failed, or the sorted
  // order could not be applied. MO2 renders the failure into the same report
  // pane ("## Loot failed to run" + the errors, loot.cpp:210-238).
  void show_error(const QString &reason);

private:
  QLabel *stage_label_ = nullptr;
  QTextEdit *report_   = nullptr;
  QPushButton *close_  = nullptr;
};

// The report body exactly as the pane renders it: MO2's
// Loot::Report::successMarkdown() (loot.cpp:240-274) without the per-plugin
// sections. Public so the mapping can be asserted without a display; the
// dialog is its only production caller.
[[nodiscard]] QString
loot_report_text(const std::vector<engine::Sorter::Loot::Result::Message> &messages,
                 const engine::Sorter::Loot::Stats &stats);

}  // namespace ui