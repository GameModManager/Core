#include "ui/widgets/instance_switcher_content_widget.h"

#include <QCheckBox>
#include <QFrame>
#include <QSizePolicy>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "engine/instance/instance.h"
#include "engine/instance/instance_utils.h"
#include "engine/instance/toml_utils.h"
#include "engine/plugin_host/plugin_loader.h"
#include "platform/platform.h"
#include "ui/settings/settings.h"
#include "ui/game_selection/game_selection_widget.h"
#include "ui/theme/icon_manager.h"
#include "ui/widgets/game_icon_cache.h"
#include "ui/widgets/smooth_scroll.h"
#include "ui/widgets/task_dialog.h"

#include <algorithm>
#include <unordered_set>
#include <filesystem>

namespace ui {

InstanceSwitcherContentWidget::InstanceSwitcherContentWidget(
    engine::PluginLoader *plugins, QWidget *parent)
    : QWidget(parent), plugins_(plugins) {
  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(16, 16, 16, 12);
  main_layout->setSpacing(12);

  // Title
  auto *title = new QLabel(tr("Select an instance"));
  {
    QFont f = title->font();
    f.setPointSize(14);
    f.setBold(true);
    title->setFont(f);
  }
  main_layout->addWidget(title);

  // Filter box
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(tr("Filter instances..."));
  main_layout->addWidget(filter_);

  // Body: list + details
  auto *body = new QHBoxLayout();
  body->setSpacing(12);

  list_ = new QListWidget(this);
  list_->setSelectionMode(QAbstractItemView::SingleSelection);
  list_->setSpacing(2);
  body->addWidget(list_, 2);

  details_ = new QWidget(this);
  details_->setMinimumWidth(200);
  auto *details_layout = new QVBoxLayout(details_);
  details_layout->setContentsMargins(8, 8, 8, 8);
  details_layout->setSpacing(8);
  details_layout->setAlignment(Qt::AlignTop);

  details_empty_ = new QLabel(tr("No instance selected."), details_);
  details_empty_->setWordWrap(true);
  details_layout->addWidget(details_empty_);

  details_form_ = new QWidget(details_);
  auto *form    = new QVBoxLayout(details_form_);
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(8);

  auto make_label = [this](const QString &text) {
    auto *lbl = new QLabel(text, details_form_);
    QFont f   = lbl->font();
    f.setBold(true);
    lbl->setFont(f);
    return lbl;
  };
  auto make_value = [this](const QString &initial) {
    auto *v = new QLabel(initial, details_form_);
    v->setWordWrap(true);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QSizePolicy pol(QSizePolicy::Expanding, QSizePolicy::Preferred);
    pol.setHeightForWidth(true);
    v->setSizePolicy(pol);
    return v;
  };
  auto add_row = [this, form, &make_label](const QString &label_text, QLabel *value) {
    auto *lbl = make_label(label_text);
    form->addWidget(lbl);
    form->addWidget(value);
    // thin separator between rows
    auto *sep = new QFrame(details_form_);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Plain);
    sep->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    form->addWidget(sep);
    form->addSpacing(4);
  };

  detail_name_value_ = make_value({});
  add_row(tr("Name"), detail_name_value_);
  detail_location_value_ = make_value({});
  add_row(tr("Location"), detail_location_value_);
  detail_game_value_ = make_value({});
  add_row(tr("Game"), detail_game_value_);
  detail_game_dir_value_ = make_value({});
  add_row(tr("Game directory"), detail_game_dir_value_);

  details_layout->addWidget(details_form_);
  details_layout->addStretch(1);

  details_form_->hide();

  body->addWidget(details_, 1);

  main_layout->addLayout(body, 1);

  // Action buttons
  auto *create_btn = new QPushButton(this);
  {
    create_btn->setIcon(engine::IconManager::instance().resolve_icon(
        QStringLiteral("document-new"), QStyle::SP_FileDialogNewFolder));
    create_btn->setText(tr("Create new instance"));
    create_btn->setObjectName("createButton");
  }
  auto *open_folder_btn = new QPushButton(tr("Open Folder"), this);
  open_folder_btn->setIcon(engine::IconManager::instance().resolve_icon(
      QStringLiteral("document-open-folder"), QStyle::SP_DirOpenIcon));
  open_folder_btn->setObjectName("openFolderButton");
  auto *open_toml_btn = new QPushButton(tr("Open instance.toml"), this);
  open_toml_btn->setIcon(engine::IconManager::instance().resolve_icon(
      QStringLiteral("text-html"), QStyle::SP_FileIcon));
  open_toml_btn->setObjectName("openTomlButton");
  auto *rename_btn = new QPushButton(tr("Rename..."), this);
  rename_btn->setIcon(engine::IconManager::instance().resolve_icon(
      QStringLiteral("document-edit"), QStyle::SP_FileIcon));
  rename_btn->setObjectName("renameButton");
  auto *duplicate_btn = new QPushButton(tr("Duplicate..."), this);
  duplicate_btn->setIcon(engine::IconManager::instance().resolve_icon(
      QStringLiteral("document-new"), QStyle::SP_FileDialogNewFolder));
  duplicate_btn->setObjectName("duplicateButton");
  auto *delete_btn = new QPushButton(tr("Delete..."), this);
  delete_btn->setIcon(engine::IconManager::instance().resolve_icon(
      QStringLiteral("edit-delete"), QStyle::SP_TrashIcon));
  delete_btn->setObjectName("deleteButton");

  auto *bottom_layout = new QHBoxLayout();
  bottom_layout->setContentsMargins(0, 0, 0, 0);
  bottom_layout->addWidget(create_btn);
  bottom_layout->addWidget(open_folder_btn);
  bottom_layout->addWidget(open_toml_btn);
  bottom_layout->addWidget(rename_btn);
  bottom_layout->addWidget(duplicate_btn);
  bottom_layout->addWidget(delete_btn);
  bottom_layout->addStretch();
  main_layout->addLayout(bottom_layout);

  // Connections
  connect(create_btn, &QPushButton::clicked, this,
          &InstanceSwitcherContentWidget::create_new_instance);
  connect(open_folder_btn, &QPushButton::clicked, this,
          &InstanceSwitcherContentWidget::open_folder_selected);
  connect(open_toml_btn, &QPushButton::clicked, this,
          &InstanceSwitcherContentWidget::open_toml_selected);
  connect(rename_btn, &QPushButton::clicked, this,
          &InstanceSwitcherContentWidget::rename_selected);
  connect(duplicate_btn, &QPushButton::clicked, this,
          &InstanceSwitcherContentWidget::duplicate_selected);
  connect(delete_btn, &QPushButton::clicked, this,
          &InstanceSwitcherContentWidget::delete_selected);
  connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
    emit_selected();
  });
  connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
    update_details();
    if (immediate_switch_)
      emit_selected();
  });
  connect(list_, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem *, QListWidgetItem *) {
            update_details();
          });

  connect(filter_, &QLineEdit::textChanged, this, [this](const QString &t) {
    filter_text_ = t;
    apply_filter();
  });

  // Swap an async-downloaded icon into its row when it lands
  connect(&GameIconCache::instance(), &GameIconCache::icon_ready, this,
          &InstanceSwitcherContentWidget::update_icons_for);

  // TODO: gate behind a Settings "Smooth scrolling" checkbox.
  if (Settings::instance().smooth_scrolling())
    ui::enable_smooth_scrolling(this);
}

void InstanceSwitcherContentWidget::set_immediate_switch(bool enabled) {
  immediate_switch_ = enabled;
}

void InstanceSwitcherContentWidget::set_active_instance(
    const std::filesystem::path &root) {
  active_root_ = root;
  refresh_list();
}

void InstanceSwitcherContentWidget::set_filter_text(const QString &text) {
  if (filter_)
    filter_->setText(text);
}

void InstanceSwitcherContentWidget::load_instances(const std::string &instances_dir) {
  instances_dir_ = instances_dir;
  refresh_list();
}

QString InstanceSwitcherContentWidget::selected_instance() const {
  auto *item = list_->currentItem();
  if (!item)
    return {};
  // find index in entries_ by stored name in item data
  QString name = item->data(Qt::UserRole).toString();
  if (!name.isEmpty())
    return name;
  int row = list_->row(item);
  if (row >= 0 && row < static_cast<int>(entries_.size()))
    return QString::fromStdString(entries_[row].name);
  return {};
}

void InstanceSwitcherContentWidget::emit_selected() {
  const QString name = selected_instance();
  if (!name.isEmpty())
    emit instance_selected(name);
}

void InstanceSwitcherContentWidget::refresh_list() {
  // remember selection
  QString prev = selected_instance();

  list_->clear();
  entries_.clear();

  std::error_code ec;
  // Roots to scan: the configured dir plus any registered extra roots.
  std::vector<std::filesystem::path> roots;
  if (!instances_dir_.empty())
    roots.push_back(std::filesystem::path(instances_dir_));
  for (const auto &r : engine::instance_roots()) {
    bool dup = false;
    for (const auto &x : roots) {
      if (x == r) {
        dup = true;
        break;
      }
    }
    if (!dup)
      roots.push_back(r);
  }

  std::unordered_set<std::string> seen_names;
  for (const auto &root : roots) {
    if (!std::filesystem::is_directory(root, ec))
      continue;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
      if (!entry.is_directory())
        continue;
      auto toml = entry.path() / "instance.toml";
      if (!std::filesystem::exists(toml))
        continue;

      const std::string name = entry.path().filename().string();
      if (!seen_names.insert(name).second)
        continue;

      InstanceSwitcherEntry ie;
      ie.name = name;
      ie.root = entry.path();

      if (auto tbl = engine::parse_instance_toml(toml)) {
        if (auto v = (*tbl)["game_id"].value<std::string>())
          ie.game_id = *v;
        if (auto v = (*tbl)["portable"].value<bool>())
          ie.portable = *v;
      }

      std::string display_name;
      if (plugins_)
        display_name = plugins_->display_name_for(ie.game_id);
      if (display_name.empty())
        display_name = ie.game_id;

      std::string label = engine::instance_display_name(entry.path());

      ie.display_name = display_name;
      entries_.push_back(ie);
    }  // directory_iterator
  }  // roots

  // natural sort by display label (case-insensitive)
  std::sort(entries_.begin(), entries_.end(),
            [](const InstanceSwitcherEntry &a, const InstanceSwitcherEntry &b) {
              std::string la = engine::instance_display_name(a.root);
              std::string lb = engine::instance_display_name(b.root);
              // case-insensitive natural-ish: lowercase compare
              std::string ca, cb;
              for (char c : la)
                ca += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
              for (char c : lb)
                cb += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
              return ca < cb;
            });

  // build items
  for (auto &ie : entries_) {
    auto *item_widget = new QWidget();
    auto *hlay        = new QHBoxLayout(item_widget);
    hlay->setContentsMargins(8, 6, 8, 6);
    hlay->setSpacing(12);

    auto *icon_label = new QLabel();
    icon_label->setFixedSize(36, 36);
    icon_label->setAlignment(Qt::AlignCenter);
    auto icon =
        GameIconCache::instance().icon_for(QString::fromStdString(ie.game_id),
                                           QString::fromStdString(ie.display_name), 36);
    icon_label->setPixmap(icon.pixmap(36, 36));
    hlay->addWidget(icon_label);
    ie.icon_label = icon_label;

    auto *text_layout = new QVBoxLayout();
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(2);

    std::string label = engine::instance_display_name(ie.root);
    bool is_active    = (!active_root_.empty() && ie.root == active_root_);
    QString name_text = QString::fromStdString(label);
    if (is_active)
      name_text += tr(" (current)");

    auto *name_label = new QLabel(name_text);
    {
      QFont f = name_label->font();
      f.setPointSize(11);
      f.setBold(true);
      if (is_active)
        f.setItalic(true);
      name_label->setFont(f);
    }
    text_layout->addWidget(name_label);

    auto *path_label = new QLabel(QString::fromStdString(ie.root.string()));
    path_label->setObjectName("pathLabel");
    path_label->setWordWrap(false);
    text_layout->addWidget(path_label);

    hlay->addLayout(text_layout, 1);

    auto *qitem = new QListWidgetItem(list_);
    qitem->setData(Qt::UserRole, QString::fromStdString(ie.name));
    qitem->setSizeHint(item_widget->sizeHint());
    list_->addItem(qitem);
    list_->setItemWidget(qitem, item_widget);
  }

  apply_filter();

  // preselect active or previous
  if (!entries_.empty()) {
    int target = 0;
    if (!active_root_.empty()) {
      for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (entries_[i].root == active_root_) {
          target = i;
          break;
        }
      }
    } else if (!prev.isEmpty()) {
      for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        if (QString::fromStdString(entries_[i].name) == prev) {
          target = i;
          break;
        }
      }
    }
    // map filtered? list_ may have hidden items; select by row in list_ (same order)
    if (target < list_->count())
      list_->setCurrentRow(target);
  }

  update_details();
}

void InstanceSwitcherContentWidget::apply_filter() {
  QString ft = filter_text_.trimmed().toLower();
  for (int i = 0; i < list_->count(); ++i) {
    auto *item   = list_->item(i);
    QString name = item->data(Qt::UserRole).toString();
    // find entry label
    std::string label;
    for (auto &e : entries_) {
      if (e.name == name.toStdString()) {
        label = engine::instance_display_name(e.root);
        break;
      }
    }
    bool visible = ft.isEmpty() ||
                   QString::fromStdString(label).toLower().contains(ft) ||
                   name.toLower().contains(ft);
    item->setHidden(!visible);
  }
}

void InstanceSwitcherContentWidget::update_details() {
  if (!details_)
    return;
  QString name = selected_instance();
  if (name.isEmpty()) {
    if (details_empty_) {
      details_empty_->setText(tr("No instance selected."));
      details_empty_->show();
    }
    if (details_form_)
      details_form_->hide();
    return;
  }
  // find entry
  InstanceSwitcherEntry *ie = nullptr;
  for (auto &e : entries_) {
    if (e.name == name.toStdString()) {
      ie = &e;
      break;
    }
  }
  if (!ie) {
    if (details_empty_) {
      details_empty_->setText(tr("No instance selected."));
      details_empty_->show();
    }
    if (details_form_)
      details_form_->hide();
    return;
  }
  // read toml for game dir
  std::string game_dir;
  if (auto tbl = engine::parse_instance_toml(ie->root / "instance.toml")) {
    if (auto v = (*tbl)["game_dir"].value<std::string>())
      game_dir = *v;
  }
  if (details_empty_)
    details_empty_->hide();
  if (details_form_)
    details_form_->show();
  if (detail_name_value_)
    detail_name_value_->setText(
        QString::fromStdString(engine::instance_display_name(ie->root)));
  if (detail_location_value_)
    detail_location_value_->setText(QString::fromStdString(ie->root.string()));
  if (detail_game_value_)
    detail_game_value_->setText(QString::fromStdString(ie->display_name));
  if (detail_game_dir_value_)
    detail_game_dir_value_->setText(QString::fromStdString(game_dir));
}

void InstanceSwitcherContentWidget::update_icons_for(const QString &game_id) {
  std::string gid = game_id.toStdString();
  for (auto &entry : entries_) {
    if (entry.game_id == gid && entry.icon_label) {
      auto icon = GameIconCache::instance().icon_for(
          QString::fromStdString(entry.game_id),
          QString::fromStdString(entry.display_name), 36);
      entry.icon_label->setPixmap(icon.pixmap(36, 36));
    }
  }
}

void InstanceSwitcherContentWidget::delete_selected() {
  QString name = selected_instance();
  if (name.isEmpty())
    return;
  // active guard
  if (!active_root_.empty()) {
    for (auto &e : entries_) {
      if (e.name == name.toStdString() && e.root == active_root_) {
        QMessageBox::information(this, tr("Delete instance"),
                                 tr("The active instance cannot be deleted."));
        return;
      }
    }
  }
  // also guard by root equality
  for (auto &e : entries_) {
    if (e.name == name.toStdString() && !active_root_.empty() &&
        e.root == active_root_) {
      QMessageBox::information(this, tr("Delete instance"),
                               tr("The active instance cannot be deleted."));
      return;
    }
  }

  // external paths
  auto ext = engine::instance_external_paths(name.toStdString());

  // TaskDialog
  ui::TaskDialog dlg(this, tr("Deleting instance"));
  dlg.main(tr("These files and folders will be deleted"));
  const auto dlg_content = ui::build_instance_delete_dialog_content(ext);
  dlg.content(tr(dlg_content.content.toStdString().c_str()));
  // checkboxes for external paths (only when externals exist)
  std::vector<QCheckBox *> cbs;
  if (dlg_content.show_external_list) {
    QWidget *cb_container = new QWidget();
    auto *cb_layout       = new QVBoxLayout(cb_container);
    for (auto &p : ext) {
      auto *cb = new QCheckBox(QString::fromStdString(p.string()), cb_container);
      cb->setChecked(false);
      cb_layout->addWidget(cb);
      cbs.push_back(cb);
    }
    dlg.add_content(cb_container);
  }

  dlg.add_button({"Move to the recycle bin", "", QMessageBox::Yes});
  dlg.add_button({"Delete permanently", "", QMessageBox::No});
  dlg.add_button({"Cancel", "", QMessageBox::Cancel});

  auto reply = dlg.exec();
  if (reply == QMessageBox::Cancel)
    return;

  bool permanent = (reply == QMessageBox::No);

  // delete external checked ones too
  for (size_t i = 0; i < ext.size() && i < cbs.size(); ++i) {
    if (cbs[i]->isChecked()) {
      engine::remove_path(ext[i], permanent);
    }
  }

  if (!engine::delete_instance(name.toStdString(), permanent)) {
    QMessageBox::warning(this, tr("Delete instance"),
                         tr("Failed to delete instance %1.").arg(name));
    return;
  }
  refresh_list();
  emit instances_changed();
}

void InstanceSwitcherContentWidget::rename_selected() {
  QString name = selected_instance();
  if (name.isEmpty())
    return;
  // find current display name
  std::string current_display;
  for (auto &e : entries_) {
    if (e.name == name.toStdString()) {
      current_display = engine::instance_display_name(e.root);
      break;
    }
  }
  QString new_name = QString::fromStdString(
      ui::prompt_instance_name(this, QString::fromStdString(current_display)));
  if (new_name.isEmpty())
    return;
  // If user typed a name that sanitizes same as existing folder? still update display
  // name. Use the returned sanitized name as new display name.
  if (!engine::rename_instance_display_name(name.toStdString(),
                                            new_name.toStdString())) {
    QMessageBox::warning(this, tr("Rename instance"),
                         tr("Failed to rename instance %1.").arg(name));
    return;
  }
  refresh_list();
  emit instances_changed();
}

void InstanceSwitcherContentWidget::duplicate_selected() {
  QString name = selected_instance();
  if (name.isEmpty())
    return;
  std::string current_display;
  for (auto &e : entries_) {
    if (e.name == name.toStdString()) {
      current_display = engine::instance_display_name(e.root);
      break;
    }
  }
  QString new_name = QString::fromStdString(ui::prompt_instance_name(
      this, QString::fromStdString(current_display + " copy")));
  if (new_name.isEmpty())
    return;

  // warn about external overrides not cloned
  auto ext = engine::instance_external_paths(name.toStdString());
  if (!ext.empty()) {
    QMessageBox::information(
        this, tr("Duplicate instance"),
        tr("External override folders are not cloned; the new instance will use "
           "default paths under its own root."));
  }

  auto inst = engine::clone_instance(name.toStdString(), new_name.toStdString());
  if (inst.info().game_id.empty() && inst.info().root.empty()) {
    QMessageBox::warning(this, tr("Duplicate instance"),
                         tr("Failed to duplicate instance %1.").arg(name));
    return;
  }
  refresh_list();
  emit instances_changed();
}

void InstanceSwitcherContentWidget::open_folder_selected() {
  QString name = selected_instance();
  if (name.isEmpty())
    return;
  for (auto &e : entries_) {
    if (e.name == name.toStdString()) {
      engine::open_with_default_handler(e.root);
      break;
    }
  }
}

void InstanceSwitcherContentWidget::open_toml_selected() {
  QString name = selected_instance();
  if (name.isEmpty())
    return;
  for (auto &e : entries_) {
    if (e.name == name.toStdString()) {
      engine::open_with_default_handler(e.root / "instance.toml");
      break;
    }
  }
}

}  // namespace ui