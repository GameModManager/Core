#include "ui/proton/proton_panel.h"

#include "engine/instance/instance.h"
#include "engine/plugin_host/plugin_loader.h"
#include "engine/proton_tools.h"
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
#include <QPushButton>
#include <QGroupBox>
#include <QVBoxLayout>

#include <cstdint>

namespace ui {

ProtonPanel::ProtonPanel(engine::PlatformInterface* platform,
                         engine::PluginLoader* plugin_loader,
                         const std::string& game_id,
                         const std::string& game_display_name,
                         const std::filesystem::path& game_dir,
                         uint32_t steam_appid,
                         const std::filesystem::path& instance_root,
                         const std::string& current_runner,
                         QWidget* parent)
    : QDialog(parent),
      platform_(platform),
      plugin_loader_(plugin_loader),
      game_id_(game_id),
      game_display_name_(game_display_name),
      game_dir_(game_dir),
      steam_appid_(steam_appid),
      instance_root_(instance_root) {
    setWindowTitle(tr("Proton Options — %1").arg(QString::fromStdString(game_display_name_)));

    auto* root = new QVBoxLayout(this);

    // --- Proton runner selector (inline, not boxed) ---
    auto* runner_row = new QHBoxLayout;
    runner_row->addWidget(new QLabel(tr("Proton runner:"), this));
    runner_combo_ = new QComboBox(this);
    runner_row->addWidget(runner_combo_, 1);
    root->addLayout(runner_row);

    runner_detail_ = new QLabel(this);
    runner_detail_->setWordWrap(true);
    runner_detail_->setTextFormat(Qt::PlainText);
    root->addWidget(runner_detail_);

    connect(runner_combo_, &QComboBox::currentIndexChanged,
            this, &ProtonPanel::update_runner_detail);

    // --- Divider between the runner selector and the packages ---
    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    root->addWidget(divider);

    // --- Recommended packages (wine.json shipped with the game plugin) ---
    auto* packages_group = new QGroupBox(tr("Recommended Wine Packages"), this);
    auto* packages_layout = new QVBoxLayout(packages_group);
    packages_layout_ = packages_layout;
    root->addWidget(packages_group);

    // --- Buttons ---
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* ok = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    root->addWidget(buttons);

    refresh_runners();
    load_recommended_packages();
}

std::string ProtonPanel::selected_runner() const {
    if (!runner_combo_) return {};
    // Item 0 is "Automatic"; everything else is a discovered runner name.
    int idx = runner_combo_->currentIndex();
    if (idx <= 0) return {};
    return runner_combo_->itemText(idx).toStdString();
}

void ProtonPanel::refresh_runners() {
    runner_combo_->clear();
    runner_combo_->addItem(tr("Automatic (Steam default)"));

    if (platform_) {
        for (const auto& version : platform_->enumerate_proton_versions()) {
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

void ProtonPanel::update_runner_detail() {
    if (!runner_detail_ || !platform_) return;

    QString resolved;
    int idx = runner_combo_->currentIndex();
    if (idx <= 0) {
        auto proton = engine::ProtonRuntime::find_proton_binary(
            platform_, steam_appid_);
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

std::filesystem::path ProtonPanel::recommended_packages_path() const {
    if (!plugin_loader_ || game_id_.empty()) return {};
    for (const auto& p : plugin_loader_->plugins()) {
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

void ProtonPanel::load_recommended_packages() {
    if (!packages_layout_) return;

    auto path = recommended_packages_path();
    if (path.empty()) {
        auto* missing = new QLabel(tr("No recommended packages defined for this game."), this);
        packages_layout_->addWidget(missing);
        return;
    }

    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly)) {
        auto* missing = new QLabel(tr("Could not read wine.json."), this);
        packages_layout_->addWidget(missing);
        return;
    }

    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        auto* missing = new QLabel(tr("wine.json is not valid JSON."), this);
        packages_layout_->addWidget(missing);
        return;
    }

    QStringList verbs;
    const auto arr = doc.object().value("packages").toArray();
    for (const auto& v : arr) {
        auto verb = v.toString();
        if (verb.isEmpty()) continue;
        verbs << verb;

        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(verb, this), 1);
        auto* install = new QPushButton(tr("Install"), this);
        connect(install, &QPushButton::clicked, this,
                [this, verb]() { install_packages({verb}); });
        row->addWidget(install);
        packages_layout_->addLayout(row);
    }

    if (verbs.isEmpty()) {
        auto* missing = new QLabel(tr("No recommended packages defined for this game."), this);
        packages_layout_->addWidget(missing);
        return;
    }

    auto* all_row = new QHBoxLayout;
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

void ProtonPanel::install_packages(const QStringList& verbs) {
    if (verbs.isEmpty()) return;

    engine::ProtonToolRequest request;
    request.platform = platform_;
    request.steam_appid = steam_appid_;
    request.game_dir = game_dir_;
    request.runner_override = selected_runner();

    std::vector<std::string> args;
    for (const auto& v : verbs) args.push_back(v.toStdString());

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

}  // namespace ui
