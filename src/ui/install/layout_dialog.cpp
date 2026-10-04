#include "ui/install/layout_dialog.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPalette>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QThread>
#include <QTreeView>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>

#include <algorithm>
#include <system_error>
#include <vector>

namespace ui {

namespace {

  // Item data roles. kPathRole is the row's absolute path, kIsDirRole whether it
  // can be expanded, kFetchedRole whether its children have been read yet.
  constexpr int kPathRole    = Qt::UserRole + 1;
  constexpr int kIsDirRole   = Qt::UserRole + 2;
  constexpr int kFetchedRole = Qt::UserRole + 3;

  // One row per entry in the extracted content, with the game's data directory
  // synthesised as the single top row. Directories list their children only when
  // the view expands them: an archive can hold tens of thousands of files and
  // this dialog only ever shows what the user actually opens.
  class ContentModel : public QStandardItemModel {
  public:
    ContentModel(std::filesystem::path root, QString root_label,
                 QObject *parent = nullptr)
        : QStandardItemModel(parent), content_root_(std::move(root)) {
      appendRow(make_item(content_root_, root_label, /*is_dir=*/true));
    }

    // The "<prefix>" row. Everything below it is real content on disk. The model
    // holds exactly that one top row, so its index IS the pseudo-root.
    bool canFetchMore(const QModelIndex &parent) const override {
      const auto *item = itemFromIndex(parent);
      if (!item || !item->data(kIsDirRole).toBool() ||
          item->data(kFetchedRole).toBool())
        return false;
      return true;
    }

    void fetchMore(const QModelIndex &parent) override {
      auto *item = itemFromIndex(parent);
      if (!item)
        return;
      item->setData(true, kFetchedRole);
      const auto dir =
          std::filesystem::path(item->data(kPathRole).toString().toStdString());

      std::error_code ec;
      std::vector<std::filesystem::path> dirs;
      std::vector<std::filesystem::path> files;
      for (auto it = std::filesystem::directory_iterator(dir, ec);
           it != std::filesystem::directory_iterator(); it.increment(ec)) {
        (it->is_directory(ec) ? dirs : files).push_back(it->path());
      }
      // readdir order is arbitrary; a tree that reshuffles between fetches makes
      // the row the user right-clicked mean something else.
      const auto by_name = [](const std::filesystem::path &a,
                              const std::filesystem::path &b) {
        return a.filename().string() < b.filename().string();
      };
      std::sort(dirs.begin(), dirs.end(), by_name);
      std::sort(files.begin(), files.end(), by_name);
      for (const auto &sub : dirs)
        item->appendRow(make_item(sub, {}, /*is_dir=*/true));
      for (const auto &file : files)
        item->appendRow(make_item(file, {}, /*is_dir=*/false));
    }

  private:
    static QStandardItem *make_item(const std::filesystem::path &entry,
                                    const QString &label, bool is_dir) {
      auto *item = new QStandardItem(
          label.isEmpty() ? QString::fromStdString(entry.filename().string()) : label);
      item->setEditable(false);
      item->setData(QString::fromStdString(entry.string()), kPathRole);
      item->setData(is_dir, kIsDirRole);
      item->setData(false, kFetchedRole);
      return item;
    }

    std::filesystem::path content_root_;
  };

  // A row's path relative to the content root. The pseudo-root row IS the content
  // root, so it answers empty - that is what "no subtree designated" means, and
  // what makes right-clicking the pseudo-root an unset.
  std::filesystem::path relative_to_root(const QStandardItemModel *model,
                                         const std::filesystem::path &root,
                                         const QModelIndex &index) {
    const auto *item = model->itemFromIndex(index);
    if (!item || !item->data(kPathRole).isValid())
      return {};
    const auto absolute =
        std::filesystem::path(item->data(kPathRole).toString().toStdString());
    return absolute.lexically_relative(root);
  }

}  // namespace

LayoutDialog::LayoutDialog(const std::filesystem::path &content_root,
                           const std::string &data_prefix,
                           std::shared_ptr<const engine::ModDataCheckerFeature> checker,
                           QWidget *parent)
    : QDialog(parent), content_root_(content_root), checker_(std::move(checker)) {
  // MO2 hands the dialog the lowercased data dir name
  // (installermanual.cpp:107), and that is the spelling the pseudo-root and
  // the context menu carry.
  prefix_ = QString::fromStdString(data_prefix).toLower();
  setWindowTitle(tr("Install Mod"));
  resize(560, 460);

  auto *layout = new QVBoxLayout(this);

  auto *caption = new QLabel(
      tr("<b>&lt;%1&gt;</b> is the base directory that will map onto the game's "
         "data directory. Right-click a folder in the tree to change it.")
          .arg(prefix_),
      this);
  caption->setWordWrap(true);
  layout->addWidget(caption);

  model_ = new ContentModel(content_root_, QString("<%1>").arg(prefix_), this);
  tree_  = new QTreeView(this);
  tree_->setModel(model_);
  tree_->setHeaderHidden(true);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tree_, &QTreeView::customContextMenuRequested, this,
          &LayoutDialog::show_menu);
  layout->addWidget(tree_, 1);

  verdict_label_ = new QLabel(this);
  verdict_label_->setWordWrap(true);
  layout->addWidget(verdict_label_);

  auto *box =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  box->button(QDialogButtonBox::Ok)->setText(tr("OK"));
  connect(box, &QDialogButtonBox::accepted, this, &LayoutDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(box);

  // The two actions belong to the dialog, not to a menu rebuilt per click, so
  // the object a test finds is the object the user's click runs.
  set_action_ = new QAction(tr("Set as <%1> directory").arg(prefix_), this);
  connect(set_action_, &QAction::triggered, this, &LayoutDialog::on_set_data_root);
  addAction(set_action_);

  unset_action_ = new QAction(tr("Unset <%1> directory").arg(prefix_), this);
  connect(unset_action_, &QAction::triggered, this, &LayoutDialog::on_unset_data_root);
  addAction(unset_action_);

  // The first level is where a wrong-layout archive differs from a right one,
  // and the pseudo-root on its own says nothing - so open it up front.
  //
  // Fetched explicitly rather than left to expand(): QTreeView::expand asks
  // canFetchMore(index.parent()), and for the top row that parent is the
  // INVALID index, which the model cannot resolve to an item. A live view
  // papers over it in its next layout pass; anything that reads the model
  // before then (a test, a paint-free context) sees an expanded row with no
  // children. Everything below this level stays lazy.
  model_->fetchMore(model_->index(0, 0));
  tree_->expand(model_->index(0, 0));
  refresh_verdict();
}

QString LayoutDialog::verdict_text() const {
  return verdict_label_ ? verdict_label_->text() : QString();
}

void LayoutDialog::refresh_verdict() {
  const auto designated =
      data_root_.empty() ? content_root_ : content_root_ / data_root_;
  verdict_ = engine::layout_verdict(designated, checker_);

  QString text;
  QColor colour;
  switch (verdict_) {
  case engine::LayoutVerdict::Valid:
    text   = tr("The content of <%1> looks valid.").arg(prefix_);
    colour = QColor(0x2f, 0x9e, 0x44);
    verdict_label_->setToolTip(
        tr("The top level of <%1> holds a directory or file type the game "
           "declares as mod data.")
            .arg(prefix_));
    break;
  case engine::LayoutVerdict::Invalid:
    text   = tr("The content of <%1> does not look valid.").arg(prefix_);
    colour = QColor(0xcc, 0x33, 0x33);
    verdict_label_->setToolTip(
        tr("The top level of <%1> holds nothing the game declares as mod data. "
           "Pick a different folder above, or continue knowing the mod will "
           "most likely not work.")
            .arg(prefix_));
    break;
  case engine::LayoutVerdict::Unknown:
    text   = tr("Cannot check the content of <%1>.").arg(prefix_);
    colour = QColor(0xb8, 0x86, 0x0b);
    verdict_label_->setToolTip(
        tr("This game declared no mod data directories or file types, so the "
           "layout cannot be checked either way."));
    break;
  }
  verdict_label_->setText(text);
  auto palette = verdict_label_->palette();
  palette.setColor(QPalette::WindowText, colour);
  verdict_label_->setPalette(palette);
  // MO2 colours the tree itself as well as the label (installdialog.cpp:95-122).
  tree_->setStyleSheet(
      QString("QTreeView { border: 2px solid %1; }").arg(colour.name()));
}

void LayoutDialog::on_set_data_root() {
  if (!tree_ || !model_)
    return;
  // Whichever directory is selected becomes the data directory, and the verdict
  // is recomputed against it - so the label answers for the tree that will be
  // installed, not for some better level deeper down.
  const auto chosen = relative_to_root(model_, content_root_, tree_->currentIndex());
  data_root_        = chosen;
  refresh_verdict();
}

void LayoutDialog::on_unset_data_root() {
  data_root_.clear();
  refresh_verdict();
}

void LayoutDialog::show_menu(const QPoint &pos) {
  if (!tree_ || !model_)
    return;
  const auto index = tree_->indexAt(pos);
  QMenu menu(this);
  set_action_->setEnabled(index.isValid() && index != model_->index(0, 0));
  unset_action_->setEnabled(!data_root_.empty());
  menu.addAction(set_action_);
  menu.addAction(unset_action_);
  tree_->setCurrentIndex(index);
  menu.exec(tree_->viewport()->mapToGlobal(pos));
}

QMessageBox *LayoutDialog::make_continue_prompt(QWidget *parent) {
  auto *box = new QMessageBox(QMessageBox::Warning, QObject::tr("Continue?"),
                              QObject::tr("This mod was probably NOT set up correctly, "
                                          "and most likely will NOT work. Correct the "
                                          "directory layout with the content tree "
                                          "first."),
                              QMessageBox::Ignore | QMessageBox::Cancel, parent);
  // Backing out is the default action. A layout the game does not recognise is
  // the outcome that breaks the install, so installing it has to be chosen on
  // purpose and Enter must not do it.
  box->setDefaultButton(QMessageBox::Cancel);
  return box;
}

void LayoutDialog::accept() {
  if (verdict_ == engine::LayoutVerdict::Invalid) {
    std::unique_ptr<QMessageBox> box(make_continue_prompt(this));
    if (box->exec() != QMessageBox::Ignore)
      return;  // Cancel or Esc: stay in the dialog, nothing was decided
  }
  QDialog::accept();
}

namespace {

  engine::LayoutDecision ask_layout_impl(
      const std::filesystem::path &content_root, const std::string &data_prefix,
      std::shared_ptr<const engine::ModDataCheckerFeature> checker, QWidget *parent) {
    LayoutDialog dialog(content_root, data_prefix, std::move(checker), parent);
    engine::LayoutDecision decision;
    if (dialog.exec() != QDialog::Accepted)
      decision.cancel = true;
    else
      decision.data_root = dialog.data_root();
    return decision;
  }

}  // namespace

engine::LayoutDecision
ask_layout(const std::filesystem::path &content_root, const std::string &data_prefix,
           std::shared_ptr<const engine::ModDataCheckerFeature> checker,
           QWidget *parent) {
  if (QThread::currentThread() == qApp->thread())
    return ask_layout_impl(content_root, data_prefix, std::move(checker), parent);
  // Marshal onto the main thread and block until the modal dialog is done.
  engine::LayoutDecision result;
  QMetaObject::invokeMethod(
      qApp,
      [&] {
        result = ask_layout_impl(content_root, data_prefix, std::move(checker), parent);
      },
      Qt::BlockingQueuedConnection);
  return result;
}

}  // namespace ui