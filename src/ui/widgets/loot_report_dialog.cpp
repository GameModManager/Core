#include "ui/widgets/loot_report_dialog.h"

#include <QFont>
#include <QLabel>
#include <QPushButton>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QVBoxLayout>

namespace ui {

namespace {

  // Qt's markdown parser turns "### General messages" into a real <h3> with
  // no anchor and no href, so the heading is NOT a link - it only looked like
  // one. What made it read as a control is that Qt paints every heading bold:
  // a line of bold text alone at the top of a pane looks like something you
  // press, and nothing else in the pane is bold. MO2 does not have that
  // problem because its report pane is a web view whose stylesheet strips the
  // bold off h1-h4 and sets their size instead
  // (references/modorganizer/src/resources/markdown.html:24-26, 38-40).
  //
  // The obvious fix - the same rules as a document default stylesheet - does
  // NOT work: Qt's markdown bakes the bold into an inline char format, which
  // wins over any stylesheet rule, so h1-h4 { font-weight: normal } is a
  // silent no-op. The heading level is recorded on the block format, so the
  // weight is cleared there instead, leaving the size MO2 also keeps.
  void unbold_headings(QTextDocument *doc) {
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
      if (block.blockFormat().headingLevel() <= 0)
        continue;
      for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        if (!frag.isValid())
          continue;
        QTextCharFormat fmt = frag.charFormat();
        fmt.setFontWeight(QFont::Normal);
        QTextCursor cursor(doc);
        cursor.setPosition(frag.position());
        cursor.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
        cursor.setCharFormat(fmt);
      }
    }
  }

  // The one way this pane writes: render, then restore the heading weight.
  // QTextBrowser rather than QTextEdit because Qt autolinks the bare URLs
  // LOOT messages routinely carry, and only the browser subclass opens them
  // - as a QTextEdit they rendered as links and swallowed the click, which is
  // the same "looks live, does nothing" defect. MO2 gets this free from its
  // web view.
  void render_report(QTextBrowser *report, const QString &markdown) {
    report->setMarkdown(markdown);
    unbold_headings(report->document());
  }

}  // namespace

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
  report_ = new QTextBrowser(this);
  report_->setReadOnly(true);
  report_->setOpenExternalLinks(true);
  render_report(report_, tr("Running LOOT..."));
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
    render_report(report_, loot_report_text(messages, stats));
}

void LootReportDialog::show_error(const QString &reason) {
  set_stage(tr("Done"));
  if (report_) {
    const QString body = QStringLiteral("## %1\n\n - %2");
    render_report(report_, body.arg(tr("Loot failed to run"), reason));
  }
}

}  // namespace ui