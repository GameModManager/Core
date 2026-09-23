#include "ui/widgets/task_dialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCommandLinkButton>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/settings/settings.h"

namespace ui {

namespace {

  QStyle::StandardPixmap standard_pixmap_for(QMessageBox::Icon icon) {
    switch (icon) {
    case QMessageBox::Question:
      return QStyle::SP_MessageBoxQuestion;
    case QMessageBox::Information:
      return QStyle::SP_MessageBoxInformation;
    case QMessageBox::Warning:
      return QStyle::SP_MessageBoxWarning;
    case QMessageBox::Critical:
      return QStyle::SP_MessageBoxCritical;
    case QMessageBox::NoIcon:
      break;
    }
    return QStyle::SP_MessageBoxQuestion;
  }

}  // namespace

TaskDialog::TaskDialog(QWidget* parent, const QString& title) : QWidget(parent) {
  setWindowTitle(title);
  setWindowFlags(Qt::Dialog);
  setWindowModality(Qt::ApplicationModal);
  setMinimumWidth(400);

  auto* outer = new QHBoxLayout(this);
  outer->setSpacing(12);

  icon_label_ = new QLabel(this);
  icon_label_->setAlignment(Qt::AlignTop);
  outer->addWidget(icon_label_);

  content_layout_ = new QVBoxLayout();
  content_layout_->setSpacing(8);
  outer->addLayout(content_layout_);

  main_label_ = new QLabel(this);
  main_label_->setWordWrap(true);
  QFont main_font = main_label_->font();
  main_font.setPointSizeF(main_font.pointSizeF() * 1.5);
  main_label_->setFont(main_font);
  content_layout_->addWidget(main_label_);

  content_label_ = new QLabel(this);
  content_label_->setWordWrap(true);
  content_label_->setVisible(false);
  content_layout_->addWidget(content_label_);

  button_layout_ = new QVBoxLayout();
  button_layout_->setSpacing(4);
  content_layout_->addLayout(button_layout_);

  remember_check_ = new QCheckBox(tr("Remember my choice"), this);
  remember_check_->setChecked(true);
  remember_check_->setVisible(false);
  remember_check_->setToolTip(
      tr("Remembered answers can be cleared with Reset Dialog Choices "
         "in the settings."));
  content_layout_->addWidget(remember_check_);

  remember_combo_ = new QComboBox(this);
  remember_combo_->setVisible(false);
  remember_combo_->setToolTip(
      tr("Remembered answers can be cleared with Reset Dialog Choices "
         "in the settings."));
  content_layout_->addWidget(remember_combo_);

  details_toggle_ = new QToolButton(this);
  details_toggle_->setText(tr("Show details"));
  details_toggle_->setCheckable(true);
  details_toggle_->setChecked(true);
  details_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  details_toggle_->setArrowType(Qt::ArrowType::DownArrow);
  details_toggle_->setVisible(false);
  content_layout_->addWidget(details_toggle_);

  details_edit_ = new QPlainTextEdit(this);
  details_edit_->setReadOnly(true);
  details_edit_->setVisible(false);
  // Button-face background from the style palette (QPalette-first, no
  // hardcoded colors), slightly smaller log-style text.
  QPalette details_palette = details_edit_->palette();
  details_palette.setColor(QPalette::Base, palette().color(QPalette::Button));
  details_edit_->setPalette(details_palette);
  QFont details_font = details_edit_->font();
  details_font.setPointSizeF(details_font.pointSizeF() * 0.9);
  details_edit_->setFont(details_font);
  const QFontMetrics metrics(details_font);
  details_edit_->setFixedHeight(metrics.lineSpacing() * 10 + 8);
  content_layout_->addWidget(details_edit_);

  connect(details_toggle_, &QToolButton::toggled, this, [this](bool on) {
    details_edit_->setVisible(on);
    details_toggle_->setArrowType(on ? Qt::ArrowType::DownArrow
                                     : Qt::ArrowType::RightArrow);
  });

  content_layout_->addStretch(1);
}

TaskDialog& TaskDialog::title(const QString& title) {
  setWindowTitle(title);
  return *this;
}

TaskDialog& TaskDialog::main(const QString& text) {
  main_label_->setText(text);
  return *this;
}

TaskDialog& TaskDialog::content(const QString& text) {
  content_label_->setText(text);
  content_label_->setVisible(!text.isEmpty());
  return *this;
}

TaskDialog& TaskDialog::details(const QString& text) {
  details_edit_->setPlainText(text);
  const bool has = !text.isEmpty();
  details_toggle_->setVisible(has);
  details_edit_->setVisible(has && details_toggle_->isChecked());
  return *this;
}

TaskDialog& TaskDialog::icon(QMessageBox::Icon icon) {
  if (icon == QMessageBox::NoIcon)
    return *this;
  const QIcon styled = style()->standardIcon(standard_pixmap_for(icon));
  icon_label_->setPixmap(styled.pixmap(32, 32));
  setWindowIcon(styled);
  return *this;
}

TaskDialog& TaskDialog::add_button(const TaskDialogButton& button) {
  buttons_.append(button);
  return *this;
}

TaskDialog& TaskDialog::remember(const QString& action, const QString& file) {
  remember_action_ = action;
  remember_file_   = file;
  if (action.isEmpty()) {
    remember_check_->setVisible(false);
    remember_combo_->setVisible(false);
    return *this;
  }
  if (file.isEmpty()) {
    remember_check_->setVisible(true);
    remember_combo_->setVisible(false);
  } else {
    remember_check_->setVisible(false);
    remember_combo_->setVisible(true);
    if (remember_combo_->count() == 0) {
      remember_combo_->addItem(tr("Always ask"));
      remember_combo_->addItem(tr("Remember my choice"));
      remember_combo_->addItem(tr("Remember my choice for %1").arg(file));
    }
  }
  return *this;
}

TaskDialog& TaskDialog::add_content(QWidget* widget) {
  // Insert injected widgets above the stretch at the end of the panel so
  // they land between the buttons and the details pane.
  content_layout_->insertWidget(content_layout_->count() - 1, widget);
  return *this;
}

TaskDialog& TaskDialog::set_minimum_width(int width) {
  setMinimumWidth(width);
  return *this;
}

QMessageBox::StandardButton TaskDialog::exec() {
  // MO2 checkMemory: a stored answer short-circuits without showing UI.
  if (!remember_action_.isEmpty()) {
    const auto stored =
        Settings::instance().dialog_choice(remember_action_, remember_file_);
    if (stored.has_value())
      return *stored;
  }

  build_buttons();

  result_   = QMessageBox::Ok;
  accepted_ = false;
  show();
  QEventLoop loop;
  loop_ = &loop;
  loop.exec();
  loop_ = nullptr;
  hide();

  const auto out = accepted_ ? result_ : QMessageBox::Cancel;
  // MO2 rememberChoice: persist only accepted answers.
  if (accepted_ && !remember_action_.isEmpty() && remember_enabled()) {
    Settings::instance().set_dialog_choice(remember_action_, remember_file_, out);
  }
  return out;
}

void TaskDialog::reject() {
  accepted_ = false;
  if (loop_ != nullptr)
    loop_->quit();
  else
    hide();
}

void TaskDialog::closeEvent(QCloseEvent* event) {
  reject();
  event->accept();
}

void TaskDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    reject();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void TaskDialog::accept_with(QMessageBox::StandardButton id) {
  result_   = id;
  accepted_ = true;
  if (loop_ != nullptr)
    loop_->quit();
  else
    hide();
}

void TaskDialog::build_buttons() {
  if (buttons_built_)
    return;
  buttons_built_ = true;

  if (!buttons_.isEmpty()) {
    for (const auto& spec : buttons_) {
      auto* link    = new QCommandLinkButton(spec.text, spec.description, this);
      const auto id = spec.id;
      connect(link, &QCommandLinkButton::clicked, this, [this, id] {
        accept_with(id);
      });
      button_layout_->addWidget(link);
    }
    return;
  }

  button_box_ = new QDialogButtonBox(QDialogButtonBox::Ok, this);
  connect(button_box_, &QDialogButtonBox::accepted, this, [this] {
    accept_with(QMessageBox::Ok);
  });
  connect(button_box_, &QDialogButtonBox::rejected, this, &TaskDialog::reject);
  content_layout_->addWidget(button_box_);
}

bool TaskDialog::remember_enabled() const {
  if (remember_action_.isEmpty())
    return false;
  if (remember_file_.isEmpty())
    return remember_check_->isChecked();
  return remember_combo_->currentIndex() > 0;
}

}  // namespace ui
