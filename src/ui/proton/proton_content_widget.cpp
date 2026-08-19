#include "ui/proton/proton_content_widget.h"

#include "engine/deploy/deploy_utils.h"
#include "engine/core/instance/instance.h"
#include "engine/deploy/launch/overlay_launcher.h"
#include "engine/pipeline/plugin_host/plugin_loader.h"
#include "engine/deploy/launch/proton_tools.h"
#include "platform/platform_interface.h"
#include "runtime/runtime.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QGroupBox>
#include <QThread>
#include <QVBoxLayout>

#include <cstdint>
#include <utility>

namespace ui {

ProtonContentWidget::ProtonContentWidget(
    engine::PlatformInterface *platform, engine::PluginLoader *plugin_loader,
    const std::string &game_id, const std::string &game_display_name,
    const std::filesystem::path &game_dir, uint32_t steam_appid,
    const std::filesystem::path &instance_root,
    const std::string &current_runner,
    const std::string &current_deploy_strategy,
    const engine::DeployConfig &deploy_config, QWidget *parent)
    : QWidget(parent),
      platform_(platform),
      plugin_loader_(plugin_loader),
      game_id_(game_id),
      game_display_name_(game_display_name),
      game_dir_(game_dir),
      steam_appid_(steam_appid),
      instance_root_(instance_root),
      current_deploy_strategy_(current_deploy_strategy),
      deploy_config_(deploy_config) {
  setMinimumWidth(420);

  auto *root = new QVBoxLayout(this);

  // --- Proton runner selector (inline, not boxed) ---
  auto *runner_row = new QHBoxLayout;
  runner_row->addWidget(new QLabel(tr("Proton runner:"), this));
  runner_combo_ = new QComboBox(this);
  runner_row->addWidget(runner_combo_, 1);
  root->addLayout(runner_row);

  runner_detail_ = new QLabel(this);
  runner_detail_->setWordWrap(true);
  runner_detail_->setTextFormat(Qt::PlainText);
  root->addWidget(runner_detail_);

  connect(runner_combo_, &QComboBox::currentIndexChanged, this,
          &ProtonContentWidget::update_runner_detail);

  // --- Divider between the runner selector and the packages ---
  auto *divider = new QFrame(this);
  divider->setFrameShape(QFrame::HLine);
  divider->setFrameShadow(QFrame::Sunken);
  root->addWidget(divider);

  // --- Recommended packages (wine.json shipped with the game plugin) ---
  auto *packages_group = new QGroupBox(tr("Recommended Wine Packages"), this);
  auto *packages_layout = new QVBoxLayout(packages_group);
  packages_layout_ = packages_layout;
  root->addWidget(packages_group);

  // --- Deploy management (symlink-deploy games only) ---
  build_deploy_management();

  // --- Inline deploy-task progress (hidden while idle) ---
  // Replaces the modal QProgressDialog the popup used: the same widget works
  // embedded in a tab, where a modal progress dialog would be wrong.
  auto *progress_row = new QHBoxLayout;
  deploy_progress_ = new QProgressBar(this);
  deploy_progress_->setRange(0, 0); // indeterminate until the first step
  deploy_progress_->setTextVisible(false);
  deploy_progress_->hide();
  progress_row->addWidget(deploy_progress_, 1);
  deploy_status_ = new QLabel(this);
  deploy_status_->setWordWrap(true);
  deploy_status_->hide();
  progress_row->addWidget(deploy_status_);
  root->addLayout(progress_row);

  // --- Buttons ---
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto *save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
  connect(buttons, &QDialogButtonBox::rejected, this,
          &ProtonContentWidget::cancel_requested);
  connect(save, &QPushButton::clicked, this,
          &ProtonContentWidget::save_requested);
  root->addWidget(buttons);

  refresh_runners();
  load_recommended_packages();
}

ProtonContentWidget::~ProtonContentWidget() {
  // Safety net: a deploy/remove task may still be finishing when the widget
  // is destroyed (tab closed mid-task). Never leave the worker running into a
  // dead widget (queued callbacks into a destroyed object are dropped).
  if (deploy_thread_) {
    deploy_thread_->quit();
    deploy_thread_->wait();
  }
}

std::string ProtonContentWidget::selected_runner() const {
  if (!runner_combo_) return {};
  // Item 0 is "Automatic"; everything else is a discovered runner name.
  int idx = runner_combo_->currentIndex();
  if (idx <= 0) return {};
  return runner_combo_->itemText(idx).toStdString();
}

void ProtonContentWidget::refresh_runners() {
  runner_combo_->clear();
  runner_combo_->addItem(tr("Automatic (Steam default)"));

  if (platform_) {
    for (const auto &version : platform_->enumerate_proton_versions()) {
      runner_combo_->addItem(QString::fromStdString(version.name));
    }
  }

  // Restore the current per-instance selection, if still discoverable.
  engine::Instance inst = engine::Instance::from_root(instance_root_);
  inst.read_toml();
  const QString current = QString::fromStdString(inst.info().proton_runner);
  if (!current.isEmpty()) {
    int idx = runner_combo_->findText(current);
    if (idx >= 0) {
      runner_combo_->setCurrentIndex(idx);
    } else if (current.contains('/')) {
      // Absolute path that isn't a known runner: show it as-is.
      runner_combo_->addItem(current);
      runner_combo_->setCurrentIndex(runner_combo_->count() - 1);
    }
  }
  update_runner_detail();
}

void ProtonContentWidget::update_runner_detail() {
  if (!runner_detail_ || !platform_) return;

  QString resolved;
  int idx = runner_combo_->currentIndex();
  if (idx <= 0) {
    auto proton =
        engine::ProtonRuntime::find_proton_binary(platform_, steam_appid_);
    if (proton.empty())
      resolved = tr("no Proton runner found — falls back to standalone Wine");
    else
      resolved = QString::fromStdString(proton.string());
  } else {
    auto name = runner_combo_->itemText(idx).toStdString();
    auto found = platform_->find_proton_named(name);
    if (found.empty())
      resolved = tr("runner not found on disk");
    else
      resolved = QString::fromStdString(found.string());
  }
  runner_detail_->setText(tr("Resolves to: %1").arg(resolved));
}

std::filesystem::path ProtonContentWidget::recommended_packages_path() const {
  if (!plugin_loader_ || game_id_.empty()) return {};
  for (const auto &p : plugin_loader_->plugins()) {
    if (p.game_id == game_id_) {
      // wine.json is shipped next to the plugin as <plugin_dir>/<game_id>/wine.json.
      auto dir = std::filesystem::path(p.path).parent_path() / game_id_;
      auto candidate = dir / "wine.json";
      if (std::filesystem::exists(candidate)) return candidate;
      return {};
    }
  }
  return {};
}

void ProtonContentWidget::load_recommended_packages() {
  if (!packages_layout_) return;

  auto path = recommended_packages_path();
  if (path.empty()) {
    auto *missing = new QLabel(tr("No recommended packages defined for this game."), this);
    packages_layout_->addWidget(missing);
    return;
  }

  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    auto *missing = new QLabel(tr("Could not read wine.json."), this);
    packages_layout_->addWidget(missing);
    return;
  }

  QJsonParseError err;
  auto doc = QJsonDocument::fromJson(file.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    auto *missing = new QLabel(tr("wine.json is not valid JSON."), this);
    packages_layout_->addWidget(missing);
    return;
  }

  QStringList verbs;
  const auto arr = doc.object().value("packages").toArray();
  for (const auto &v : arr) {
    auto verb = v.toString();
    if (verb.isEmpty()) continue;
    verbs << verb;

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(verb, this), 1);
    auto *install = new QPushButton(tr("Install"), this);
    connect(install, &QPushButton::clicked, this,
            [this, verb]() { install_packages({verb}); });
    row->addWidget(install);
    packages_layout_->addLayout(row);
  }

  if (verbs.isEmpty()) {
    auto *missing = new QLabel(tr("No recommended packages defined for this game."), this);
    packages_layout_->addWidget(missing);
    return;
  }

  auto *all_row = new QHBoxLayout;
  all_row->addStretch(1);
  install_all_btn_ = new QPushButton(tr("Install all recommended packages"), this);
  connect(install_all_btn_, &QPushButton::clicked, this, [this, verbs]() {
    install_packages(verbs);
  });
  all_row->addWidget(install_all_btn_);
  packages_layout_->addLayout(all_row);

  packages_status_ = new QLabel(this);
  packages_status_->setWordWrap(true);
  packages_layout_->addWidget(packages_status_);

  // Warn up front when nothing can actually install these.
  engine::ProtonToolRequest request;
  request.platform = platform_;
  request.steam_appid = steam_appid_;
  request.game_dir = game_dir_;
  if (!engine::proton_tooling_available(request)) {
    packages_status_->setText(
        tr("protontricks is not installed — recommended packages cannot be installed."));
  }
}

void ProtonContentWidget::install_packages(const QStringList &verbs) {
  if (verbs.isEmpty()) return;

  engine::ProtonToolRequest request;
  request.platform = platform_;
  request.steam_appid = steam_appid_;
  request.game_dir = game_dir_;
  request.runner_override = selected_runner();

  std::vector<std::string> args;
  for (const auto &v : verbs) args.push_back(v.toStdString());

  int64_t pid = engine::run_proton_tool(request, args);
  if (pid < 0) {
    QMessageBox::warning(this, tr("Proton Options"),
                         tr("No protontricks / winetricks available to install packages.\n"
                            "Install protontricks to manage Proton prefixes."));
    return;
  }
  if (packages_status_) {
    packages_status_->setText(
        tr("Started installing: %1").arg(verbs.join(", ")));
  }
}

void ProtonContentWidget::build_deploy_management() {
  auto *root = qobject_cast<QVBoxLayout *>(this->layout());
  if (!root) return;

  auto *group = new QGroupBox(tr("Deploy Management"), this);
  auto *layout = new QVBoxLayout(group);

  // Deployment strategy selector. Only the strategies this build actually
  // supports are listed: Symlink (direct links into the game folder) is
  // always available; OverlayFS (staging + sandboxed launch) only when the
  // host supports the overlay launcher. Hardlink is not wired into any
  // deploy path, so it is intentionally not offered.
  auto *strategy_row = new QHBoxLayout;
  strategy_row->addWidget(new QLabel(tr("Deployment strategy:"), group));
  deploy_strategy_combo_ = new QComboBox(group);
  deploy_strategy_combo_->addItem(tr("Symlink"),
                                  QString::fromLatin1(engine::kDefaultDeployStrategy));
  deploy_strategy_combo_->addItem(
      tr("Direct"),
      QString::fromLatin1(engine::kDeployStrategyDirect));
  if (engine::OverlayFsLauncher::is_supported(instance_root_ / "overwrite")) {
    deploy_strategy_combo_->addItem(
        tr("OverlayFS"),
        QString::fromLatin1(engine::kDeployStrategyOverlayFs));
  }
  int idx = deploy_strategy_combo_->findData(
      QString::fromStdString(current_deploy_strategy_));
  deploy_strategy_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
  current_deploy_strategy_ =
      deploy_strategy_combo_->currentData().toString().toStdString();
  connect(deploy_strategy_combo_, &QComboBox::currentIndexChanged, this,
          [this](int) {
            const std::string value =
                deploy_strategy_combo_->currentData().toString().toStdString();
            engine::Instance inst = engine::Instance::from_root(instance_root_);
            inst.read_toml();
            inst.write_key("deploy_strategy", value);
            current_deploy_strategy_ = value;
            update_deploy_actions_enabled();
          });
  strategy_row->addWidget(deploy_strategy_combo_, 1);
  layout->addLayout(strategy_row);

  auto *row = new QHBoxLayout;
  redeploy_btn_ = new QPushButton(tr("Force re-deploy links"), this);
  remove_btn_ = new QPushButton(tr("Remove deployed files"), this);
  connect(redeploy_btn_, &QPushButton::clicked, this,
          [this]() { run_deploy_task(DeployTaskKind::Redeploy); });
  connect(remove_btn_, &QPushButton::clicked, this,
          [this]() { run_deploy_task(DeployTaskKind::Remove); });
  row->addWidget(redeploy_btn_);
  row->addWidget(remove_btn_);
  layout->addLayout(row);

  root->addWidget(group);

  update_deploy_actions_enabled();
}

void ProtonContentWidget::update_deploy_actions_enabled() {
  // The direct-deploy actions (re-deploy / remove links) only make sense for
  // strategies that touch game_dir: Symlink and Direct. OverlayFS never
  // touches game_dir. While a task runs, the whole deploy section is locked
  // (the popup's modal progress dialog used to block interaction the same
  // way).
  const bool direct =
      (current_deploy_strategy_ == engine::kDefaultDeployStrategy) ||
      (current_deploy_strategy_ == engine::kDeployStrategyDirect);
  const bool enabled = !deploy_task_running_ && direct;
  if (redeploy_btn_) redeploy_btn_->setEnabled(enabled);
  if (remove_btn_) remove_btn_->setEnabled(enabled);
  if (deploy_strategy_combo_)
    deploy_strategy_combo_->setEnabled(!deploy_task_running_);
}

void ProtonContentWidget::run_deploy_task(DeployTaskKind kind) {
  if (deploy_thread_) return; // a task is already running

  const bool remove_only = (kind == DeployTaskKind::Remove);
  const QString confirm = remove_only
      ? tr("Remove all deployed files and restore the original game files?\n\n"
           "Every deployed link/copy is deleted and any original file parked "
           "in Original_Files is moved back. The game returns to its pristine, "
           "unmodded state.")
      : tr("Force re-deploy of all mod links?\n\n"
           "The current deploy is first removed (original game files are "
           "restored), then every enabled mod is re-deployed from scratch.");
  if (QMessageBox::question(this, tr("Deploy Management"), confirm) !=
      QMessageBox::Yes) {
    return;
  }

  deploy_task_running_ = true;
  update_deploy_actions_enabled();

  // Inline progress (tab-friendly replacement for the modal QProgressDialog).
  deploy_progress_->setRange(0, 0);
  deploy_progress_->setValue(0);
  deploy_status_->setText(remove_only ? tr("Removing deployed files...")
                                      : tr("Re-deploying mod links..."));
  deploy_progress_->show();
  deploy_status_->show();

  const engine::DeployConfig config = deploy_config_;
  auto *thread = QThread::create([this, kind, config, remove_only]() {
    const auto on_progress = [this](int done, int total) {
      QMetaObject::invokeMethod(
          this,
          [this, done, total]() {
            if (!deploy_progress_) return;
            if (total > 0) {
              deploy_progress_->setRange(0, total);
              deploy_progress_->setValue(done);
            }
          },
          Qt::QueuedConnection);
    };
    bool ok;
    if (remove_only) {
      ok = engine::remove_deployed_files(
          config.game_dir, config.backup_root, config.ledger_file, 0,
          on_progress);
    } else {
      ok = engine::remove_deployed_files(
          config.game_dir, config.backup_root, config.ledger_file, 0,
          on_progress);
      if (ok) {
        ok = engine::deploy_all_enabled_mods_direct(
            config.mods_dir, config.game_dir, config.deploy_prefix,
            config.deploy_include_mod_id, config.disable_mechanism,
            config.case_sensitive, config.ledger_file, config.backup_root, 0,
            on_progress);
      }
    }
    QMetaObject::invokeMethod(this, [this, kind, ok]() {
      finish_deploy_task(kind, ok);
    }, Qt::QueuedConnection);
  });
  thread->setObjectName(QStringLiteral("gmm-deploy-management"));
  deploy_thread_ = thread;
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();
}

void ProtonContentWidget::finish_deploy_task(DeployTaskKind kind, bool ok) {
  deploy_thread_ = nullptr;
  deploy_task_running_ = false;
  deploy_progress_->hide();
  deploy_status_->hide();
  deploy_progress_->setRange(0, 0);
  update_deploy_actions_enabled();

  const QString text = ok
      ? (kind == DeployTaskKind::Remove
             ? tr("Deployed files removed and original game files restored.")
             : tr("All enabled mods re-deployed."))
      : (kind == DeployTaskKind::Remove
             ? tr("Removal finished with errors - see the log.")
             : tr("Re-deploy finished with errors - see the log."));
  QMessageBox::information(this, tr("Deploy Management"), text);
}

} // namespace ui