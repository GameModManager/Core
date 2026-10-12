#include "ui/widgets/loot_report_dialog.h"

#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ui {

QString
loot_report_text(const std::vector<engine::Sorter::Loot::Result::Message> &messages,
                 const engine::Sorter::Loot::Stats &stats) {
  QString out;

  // MO2 Loot::Report::successMarkdown (loot.cpp:240-274). Per-message prefixes
  // are MO2's Loot::Message::toMarkdown (loot.cpp:391-412): error and warning
  // are named, info is bare.
  if (!messages.empty()) {
    out += "### " + QObject::tr("General messages") + "\n";
    for (const auto &message : messages) {
      QString text = QString::fromStdString(message.text);
      if (message.level == "error")
        text = "**" + QObject::tr("Error") + "**: " + text;
      else if (message.level == "warning")
        text = "**" + QObject::tr("Warning") + "**: " + text;
      out += " - " + text + "\n";
    }
  }
  if (out.isEmpty()) {
    out += "**" + QObject::tr("No messages.") + "**\n";
  }

  // MO2 Loot::Stats::toMarkdown (loot.cpp:303-308).
  out += QStringLiteral("`stats: %1s, lootcli %2, loot %3`")
             .arg(QString::number(stats.time / 1000.0, 'f', 2),
                  QString::fromStdString(stats.lootcli_version),
                  QString::fromStdString(stats.loot_version));
  return out;
}

LootReportDialog::LootReportDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("LOOT"));
  resize(650, 450);

  auto *layout = new QVBoxLayout(this);

  // MO2 shows one progress line above the report pane for the whole run
  // (lootdialog.cpp:93-94 progressText), starting at "Running LOOT..."
  // (lootdialog.cpp:221-223).
  stage_label_ = new QLabel(tr("Running LOOT..."), this);
  layout->addWidget(stage_label_);

  // Qt's own markdown parser, the same path QLabel::setMarkdown() takes. No
  // web engine, so nothing here depends on QWebEngineWidgets the way MO2's
  // QWebEngineView pane does.
  report_ = new QTextEdit(this);
  report_->setReadOnly(true);
  report_->setMarkdown(tr("Running LOOT..."));
  layout->addWidget(report_, 1);

  close_ = new QPushButton(tr("Close"), this);
  close_->setAutoDefault(false);
  connect(close_, &QPushButton::clicked, this, &QDialog::accept);
  layout->addWidget(close_, 0, Qt::AlignRight);
}

void LootReportDialog::set_stage(const QString &stage) {
  if (stage_label_)
    stage_label_->setText(stage);
}

void LootReportDialog::show_report(
    const std::vector<engine::Sorter::Loot::Result::Message> &messages,
    const engine::Sorter::Loot::Stats &stats) {
  set_stage(tr("Done"));
  if (report_)
    report_->setMarkdown(loot_report_text(messages, stats));
}

void LootReportDialog::show_error(const QString &reason) {
  set_stage(tr("Done"));
  if (report_)
    report_->setMarkdown(
        QStringLiteral("## %1\n\n - %2").arg(tr("Loot failed to run"), reason));
}

}  // namespace ui