#include "ui/modpack/modpack_install_wizard.h"

#include <algorithm>
#include <variant>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <system_error>

#include "engine/collection/download_router.h"
#include "engine/collection/nexus/adapter.h"
#include "engine/core/log/logger.h"
#include "engine/gmmpack/unpacker.h"
#include "engine/mod/model/mod.h"
#include "engine/pipeline/pipeline.h"
#include "engine/source/loverslab/provider.h"
#include "engine/source/modl/provider.h"
#include "engine/source/modpub/provider.h"
#include "engine/source/nexus/provider.h"
#include "engine/source/registry.h"

namespace ui {
namespace {

using engine::gmmpack::Gmmpack;
using engine::gmmpack::ModCategory;
using engine::gmmpack::ModEntry;

QString source_provider(const ModEntry& mod) {
    return std::visit(
        [](const auto& src) -> QString {
            return QString::fromStdString(src.provider);
        },
        mod.source);
}

QString source_detail(const ModEntry& mod) {
    return std::visit(
        [](const auto& src) -> QString {
            using T = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<T, engine::gmmpack::ModSourceNexus>) {
                QString s = QString::fromStdString(src.game_domain) +
                            QStringLiteral(" / ") + QString::number(src.mod_id);
                if (src.file_name)
                    s += QStringLiteral(" (") +
                         QString::fromStdString(*src.file_name) +
                         QStringLiteral(")");
                return s;
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceDirect>) {
                return QString::fromStdString(src.url);
            } else if constexpr (std::is_same_v<
                                     T,
                                     engine::gmmpack::ModSourceSteamWorkshop>) {
                return QStringLiteral("app ") +
                       QString::number(src.app_id) + QStringLiteral(" item ") +
                       QString::number(src.workshop_item_id);
            } else {
                return QString::fromStdString(src.resolution);
            }
        },
        mod.source);
}

std::optional<int64_t> source_file_size(const ModEntry& mod) {
    return std::visit(
        [](const auto& src) -> std::optional<int64_t> {
            using T = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<T, engine::gmmpack::ModSourceNexus>) {
                return src.file_size;
            } else {
                return std::nullopt;
            }
        },
        mod.source);
}

QString format_size(std::optional<int64_t> bytes) {
    if (!bytes) return ModpackInstallWizard::tr("unknown size");
    const double b = static_cast<double>(*bytes);
    if (b < 1024.0) return QStringLiteral("%1 B").arg(*bytes);
    if (b < 1024.0 * 1024.0)
        return QStringLiteral("%1 KiB").arg(b / 1024.0, 0, 'f', 1);
    if (b < 1024.0 * 1024.0 * 1024.0)
        return QStringLiteral("%1 MiB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GiB")
        .arg(b / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString category_name(ModCategory category) {
    switch (category) {
        case ModCategory::Required: return ModpackInstallWizard::tr("Required");
        case ModCategory::Recommended:
            return ModpackInstallWizard::tr("Recommended");
        case ModCategory::Optional: return ModpackInstallWizard::tr("Optional");
    }
    return {};
}

const ModEntry* find_mod(const Gmmpack& pack, const std::string& id) {
    for (const auto& mod : pack.mods) {
        if (mod.id == id) return &mod;
    }
    return nullptr;
}

QString status_icon(const QString& status) {
    if (status == QStringLiteral("downloaded")) return QStringLiteral("\u2713");
    if (status == QStringLiteral("downloading"))
        return QStringLiteral("\u25C9");
    if (status == QStringLiteral("failed")) return QStringLiteral("\u2717");
    if (status == QStringLiteral("skipped")) return QStringLiteral("\u2013");
    return QStringLiteral("\u25CB");
}

QWidget* make_empty_note(const QString& text) {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addStretch(1);
    auto* label = new QLabel(text, page);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch(1);
    return page;
}

QScrollArea* wrap_scroll(QWidget* inner) {
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    return scroll;
}

// ---------------------------------------------------------------------------
// Real download pipeline (step 5).
//
// Routing reuses engine::Collection's download router: Nexus entries consult
// the stored account status (API key present = authenticated, premium tier
// = auto-download), everything else routes on its declared resolution.
// Auto entries fetch through the SourceRegistry providers - the same
// Interface the main-window pipeline drives. Browser entries open the file
// page for a manual fetch; external-client entries (Steam Workshop) install
// outside GMM entirely.
// ---------------------------------------------------------------------------

namespace Collection = engine::Collection;

Collection::SourceResolution declared_resolution(const std::string& raw) {
    if (raw == "browser") return Collection::SourceResolution::Browser;
    if (raw == "client-subscription")
        return Collection::SourceResolution::ClientSubscription;
    return Collection::SourceResolution::Api;
}

struct DownloadRoute {
    Collection::DownloadPath path = Collection::DownloadPath::Auto;
    QString reason;
    QString open_url;  // set for Browser rows that have a page to open
};

QString nexus_file_page(const engine::gmmpack::ModSourceNexus& src) {
    QString url = QStringLiteral("https://www.nexusmods.com/") +
                  QString::fromStdString(src.game_domain) +
                  QStringLiteral("/mods/") + QString::number(src.mod_id) +
                  QStringLiteral("?tab=files");
    return url;
}

DownloadRoute route_for(const ModEntry& mod) {
    DownloadRoute route;
    std::visit(
        [&](const auto& src) {
            using T = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<T,
                                         engine::gmmpack::ModSourceNexus>) {
                const Collection::RouteOutcome outcome =
                    Collection::route_download(
                        declared_resolution(src.resolution),
                        Collection::Nexus::account_status(),
                        Collection::capabilities_for(
                            Collection::SourceNexus::kProvider));
                route.path = outcome.path;
                route.reason = QString::fromStdString(outcome.reason);
                if (route.path == Collection::DownloadPath::Browser)
                    route.open_url = nexus_file_page(src);
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceDirect>) {
                const Collection::RouteOutcome outcome =
                    Collection::route_download(
                        declared_resolution(src.resolution),
                        Collection::AccountStatus{},
                        Collection::capabilities_for(
                            Collection::SourceDirect::kProvider));
                route.path = outcome.path;
                route.reason = QString::fromStdString(outcome.reason);
                if (route.path == Collection::DownloadPath::Browser)
                    route.open_url = QString::fromStdString(src.url);
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceModPub>) {
                const Collection::RouteOutcome outcome =
                    Collection::route_download(
                        declared_resolution(src.resolution),
                        Collection::AccountStatus{},
                        Collection::capabilities_for(
                            Collection::SourceModPub::kProvider));
                route.path = outcome.path;
                route.reason = QString::fromStdString(outcome.reason);
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceLoversLab>) {
                // LoversLab declares browser-only resolution (no public API).
                const Collection::RouteOutcome outcome =
                    Collection::route_download(
                        Collection::SourceResolution::Browser,
                        Collection::AccountStatus{},
                        Collection::capabilities_for(
                            Collection::SourceLoversLab::kProvider));
                route.path = outcome.path;
                route.reason = QString::fromStdString(outcome.reason);
                // Numeric file ids map to their canonical file page.
                std::visit(
                    [&](const auto& id) {
                        using I = std::decay_t<decltype(id)>;
                        QString num;
                        if constexpr (std::is_same_v<I, int64_t>) {
                            num = QString::number(id);
                        } else {
                            bool numeric = !id.empty();
                            for (char c : id)
                                numeric = numeric && std::isdigit(
                                                         static_cast<unsigned char>(
                                                             c));
                            if (numeric) num = QString::fromStdString(id);
                        }
                        if (!num.isEmpty()) {
                            route.open_url =
                                QStringLiteral(
                                    "https://www.loverslab.com/files/file/") +
                                num + QStringLiteral("/");
                        }
                    },
                    src.mod_id);
            } else {
                // Steam Workshop: subscription lives in the Steam client.
                route.path = Collection::DownloadPath::ExternalClient;
                route.reason = QStringLiteral(
                    "source downloads through an external client subscription");
            }
        },
        mod.source);
    return route;
}

// Build the pipeline-ready engine::Mod for an Auto-routed entry. nullopt
// for Browser/ExternalClient entries (nothing to fetch).
std::optional<engine::Mod> build_engine_mod(const ModEntry& mod) {
    engine::Mod out;
    out.id = mod.id;
    out.name = mod.name.empty() ? mod.id : mod.name;
    const bool mapped = std::visit(
        [&](const auto& src) {
            using T = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<T,
                                         engine::gmmpack::ModSourceNexus>) {
                out.download_source_type = "nexus";
                out.download_source_id = std::to_string(src.mod_id);
                out.download_nxm.file_id = src.file_id.value_or(0);
                out.download_nxm.nexus_domain = src.game_domain;
                if (src.version) out.version = *src.version;
                if (src.file_name && !src.file_name->empty())
                    out.name = *src.file_name;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceDirect>) {
                out.download_source_type = "direct";
                out.download_url = src.url;
                if (src.version) out.version = *src.version;
                if (src.file_name && !src.file_name->empty())
                    out.name = *src.file_name;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceModPub>) {
                out.download_source_type = "modpub";
                std::visit(
                    [&](const auto& id) {
                        using I = std::decay_t<decltype(id)>;
                        if constexpr (std::is_same_v<I, int64_t>) {
                            out.download_source_id = std::to_string(id);
                        } else {
                            out.download_source_id = id;
                        }
                    },
                    src.mod_id);
                if (src.version) out.version = *src.version;
                if (src.file_name && !src.file_name->empty())
                    out.name = *src.file_name;
                return true;
            } else if constexpr (std::is_same_v<
                                     T, engine::gmmpack::ModSourceLoversLab>) {
                out.download_source_type = "loverslab";
                std::visit(
                    [&](const auto& id) {
                        using I = std::decay_t<decltype(id)>;
                        if constexpr (std::is_same_v<I, int64_t>) {
                            out.download_source_id = std::to_string(id);
                        } else {
                            out.download_source_id = id;
                        }
                    },
                    src.mod_id);
                if (src.version) out.version = *src.version;
                if (src.file_name && !src.file_name->empty())
                    out.name = *src.file_name;
                return true;
            }
            return false;
        },
        mod.source);
    if (!mapped) return std::nullopt;
    return out;
}

// The wizard runs outside MainWindow, so its SourceRegistry may not have
// providers yet. Register the fetch-capable set once (guarded - the main
// window registers the same providers at startup).
void ensure_download_providers() {
    auto& registry = engine::Source::Registry::instance();
    if (registry.provider_for("nexus") == nullptr)
        registry.register_provider(
            std::make_unique<engine::Source::Nexus::Provider>());
    if (registry.provider_for("loverslab") == nullptr)
        registry.register_provider(
            std::make_unique<engine::Source::LoversLab::Provider>());
    if (registry.provider_for("direct") == nullptr)
        registry.register_provider(
            std::make_unique<engine::Source::Modl::Provider>());
    if (registry.provider_for("modpub") == nullptr)
        registry.register_provider(
            std::make_unique<engine::Source::ModPub::Provider>());
}

// One fetch on a worker thread: provider lookup, archive-name resolution,
// resume-aware fetch into dest_dir, then a queued completion callback.
// No Q_OBJECT needed - results travel back via queued functor invokes.
class FetchThread : public QThread {
public:
    using ProgressFn = std::function<void(int64_t, int64_t)>;
    using MetaFn = std::function<void(const std::string&, const std::string&)>;
    using DoneFn =
        std::function<void(bool, const std::string&, const std::string&)>;

    FetchThread(QString id, engine::Mod mod, std::filesystem::path dest_dir,
                std::atomic_bool* cancel, ProgressFn on_progress,
                MetaFn on_meta, DoneFn on_done, QObject* parent = nullptr)
        : QThread(parent),
          id_(std::move(id)),
          mod_(std::move(mod)),
          dest_dir_(std::move(dest_dir)),
          cancel_(cancel),
          on_progress_(std::move(on_progress)),
          on_meta_(std::move(on_meta)),
          on_done_(std::move(on_done)) {}

    void run() override {
        auto* provider = engine::Source::Registry::instance().provider_for(
            mod_.download_source_type);
        if (provider == nullptr) {
            on_done_(false, {},
                     "no download provider for source type '" +
                         mod_.download_source_type + "'");
            return;
        }
        const engine::Source::SourceDownloadInfo info =
            provider->resolve_download_info(mod_);
        on_meta_(info.archive_name, info.display_name);

        std::string filename = info.archive_name;
        if (filename.empty()) {
            filename = mod_.download_source_id;
            if (mod_.download_nxm.file_id > 0)
                filename += "-" + std::to_string(mod_.download_nxm.file_id);
            filename += ".zip";
        }
        if (!info.display_name.empty()) mod_.name = info.display_name;
        mod_.archive_filename = filename;

        std::error_code ec;
        std::filesystem::create_directories(dest_dir_, ec);
        if (ec) {
            on_done_(false, {},
                     "cannot create downloads dir: " + ec.message());
            return;
        }
        const std::filesystem::path dest = dest_dir_ / filename;

        engine::PipelineContext ctx;
        ctx.download_resume_from = 0;
        if (std::filesystem::exists(dest, ec)) {
            const auto size = std::filesystem::file_size(dest, ec);
            if (!ec && size > 0)
                ctx.download_resume_from = static_cast<int64_t>(size);
        }
        ctx.should_abort = [this]() {
            return cancel_ != nullptr && cancel_->load();
        };
        ctx.on_progress = [this](int64_t downloaded, int64_t total,
                                 double /*speed*/) {
            on_progress_(downloaded, total);
        };

        const bool ok = provider->fetch(mod_, ctx, dest);
        if (!ok) {
            const std::string reason =
                ctx.download_paused ? "download paused" : "download failed";
            on_done_(false, {}, reason);
            return;
        }
        if (!std::filesystem::exists(dest, ec)) {
            on_done_(false, {}, "provider reported success but produced no file");
            return;
        }
        on_done_(true, dest.string(), {});
    }

private:
    QString id_;
    engine::Mod mod_;
    std::filesystem::path dest_dir_;
    std::atomic_bool* cancel_;
    ProgressFn on_progress_;
    MetaFn on_meta_;
    DoneFn on_done_;
};

}  // namespace

ModpackInstallWizard::ModpackInstallWizard(engine::gmmpack::Gmmpack pack,
                                           Mode mode, QWidget* parent)
    : QDialog(parent), pack_(std::move(pack)), mode_(mode) {
    const QString pack_name =
        QString::fromStdString(pack_.manifest.info.name);
    setWindowTitle(pack_name.isEmpty()
                       ? tr("Install Modpack")
                       : tr("Install Modpack: %1").arg(pack_name));
    resize(900, 600);
    setMinimumSize(720, 480);

    build_steps();

    // Seed persisted step state from pack defaults.
    for (const auto& group : pack_.manifest.choice_groups) {
        const QString gid = QString::fromStdString(group.id);
        if (group.mode == "exactly-one" && !group.member_mod_ids.empty()) {
            choice_selections_[gid] = QStringList(
                QString::fromStdString(group.member_mod_ids.front()));
        } else {
            choice_selections_[gid] = QStringList();
        }
    }
    for (const auto& entry : pack_.ini_edits) {
        for (const auto& tweak : entry.tweaks) {
            ini_enabled_[QString::fromStdString(tweak.id)] = tweak.enabled;
        }
    }
    for (const auto& mod : pack_.mods) {
        download_status_[QString::fromStdString(mod.id)] =
            QStringLiteral("pending");
    }
    rebuild_patch_plan();
    for (const auto& exe : pack_.executables) {
        if (exe.role == "setup" && exe.auto_run) {
            tool_status_[QString::fromStdString(exe.id)] =
                QStringLiteral("pending");
        }
    }

    auto* layout = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter);

    sidebar_ = new QListWidget(splitter);
    sidebar_->setMinimumWidth(140);
    sidebar_->setMaximumWidth(220);
    for (const auto& step : steps_) {
        auto* item = new QListWidgetItem(step.title, sidebar_);
        item->setData(Qt::UserRole, static_cast<int>(step.id));
        if (step.skipped) {
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable &
                           ~Qt::ItemIsEnabled);
        }
    }
    splitter->addWidget(sidebar_);
    connect(sidebar_, &QListWidget::itemClicked, this,
            &ModpackInstallWizard::on_step_clicked);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(right);
    build_pages();
    right_layout->addWidget(stack_, 1);

    auto* nav = new QHBoxLayout();
    nav->addStretch(1);
    auto* cancel_button = new QPushButton(tr("Cancel"), right);
    back_button_ = new QPushButton(tr("< Back"), right);
    next_button_ = new QPushButton(tr("Next >"), right);
    next_button_->setDefault(true);
    nav->addWidget(cancel_button);
    nav->addWidget(back_button_);
    nav->addWidget(next_button_);
    right_layout->addLayout(nav);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 7);

    connect(back_button_, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_back);
    connect(next_button_, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_next);
    connect(cancel_button, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_cancel);

    sim_timer_ = new QTimer(this);
    sim_timer_->setSingleShot(false);

    go_to(0);
    refresh_chrome();
}

ModpackInstallWizard::~ModpackInstallWizard() {
    // Abort an in-flight fetch and wait for the worker thread before any
    // member (notably the cancel flag) goes away. Providers poll the flag
    // per chunk, so the wait is short; resume data stays on disk.
    fetch_cancel_.store(true);
    if (fetch_thread_ != nullptr) {
        fetch_thread_->wait();
        delete fetch_thread_;
        fetch_thread_ = nullptr;
    }
}

// Validated patch plan: group the pack's raw patches into per-mod chains
// (sequence-sorted, contiguity-checked). Chains still display when
// validation reports errors so consent stays usable; the errors surface as
// a warning above the table.
void ModpackInstallWizard::rebuild_patch_plan() {
    auto [chains, diagnostics] =
        engine::gmmpack::build_patch_chains(pack_.patches);
    patch_chains_ = std::move(chains);
    QStringList errors;
    for (const auto& d : diagnostics) {
        if (d.severity == engine::gmmpack::Diagnostic::Severity::Error) {
            errors << QString::fromStdString(d.path + ": " + d.message);
        }
    }
    patch_plan_error_ = errors.join(QStringLiteral("\n"));
    patch_allowed_.clear();
    for (const auto& chain : patch_chains_) {
        patch_allowed_[QString::fromStdString(chain.mod_id)] =
            false;  // Deny by default.
    }
}

std::vector<QString> ModpackInstallWizard::ordered_download_ids() const {    std::vector<const ModEntry*> ordered;
    for (const auto& mod : pack_.mods) ordered.push_back(&mod);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ModEntry* a, const ModEntry* b) {
                         return a->phase < b->phase;
                     });
    std::vector<QString> ids;
    ids.reserve(ordered.size());
    for (const ModEntry* mod : ordered)
        ids.push_back(QString::fromStdString(mod->id));
    return ids;
}

void ModpackInstallWizard::pump_download_queue() {
    if (fetch_thread_ != nullptr) return;  // one in-flight fetch at a time
    while (!download_queue_.empty()) {
        const QString next = download_queue_.front();
        download_queue_.pop_front();
        const QString status = download_status_.value(next);
        if (status != QStringLiteral("pending") &&
            status != QStringLiteral("failed")) {
            continue;  // skipped or finished while queued
        }
        const ModEntry* mod = find_mod(pack_, next.toStdString());
        if (mod == nullptr) continue;
        if (route_for(*mod).path != Collection::DownloadPath::Auto) {
            continue;  // browser/external rows never auto-fetch
        }
        start_fetch(next);
        return;
    }
    refresh_downloads_ui();
}

void ModpackInstallWizard::start_fetch(const QString& mod_id) {
    const ModEntry* mod = find_mod(pack_, mod_id.toStdString());
    if (mod == nullptr) return;
    const std::optional<engine::Mod> engine_mod = build_engine_mod(*mod);
    if (!engine_mod.has_value()) return;
    ensure_download_providers();

    QString dest_dir = downloads_dir();
    if (dest_dir.isEmpty()) {
        QString base = mods_dir();
        if (base.isEmpty()) base = instance_root();
        dest_dir = base.isEmpty() ? QStringLiteral(".")
                                  : base + QStringLiteral("/../downloads");
    }

    download_status_[mod_id] = QStringLiteral("downloading");
    download_error_.remove(mod_id);
    download_fraction_[mod_id] = 0.0;
    active_download_id_ = mod_id;
    fetch_cancel_.store(false);
    refresh_downloads_ui();

    // Callbacks marshal back onto this (UI) thread; the fetch runs on its
    // own thread through the SourceRegistry provider.
    auto on_progress = [this, mod_id](int64_t downloaded, int64_t total) {
        QMetaObject::invokeMethod(
            this,
            [this, mod_id, downloaded, total]() {
                on_fetch_progress(mod_id, downloaded, total);
            },
            Qt::QueuedConnection);
    };
    auto on_meta = [this, mod_id](const std::string& archive,
                                  const std::string& display) {
        QMetaObject::invokeMethod(
            this,
            [this, mod_id, archive, display]() {
                on_fetch_meta(mod_id, QString::fromStdString(archive),
                              QString::fromStdString(display));
            },
            Qt::QueuedConnection);
    };
    auto on_done = [this, mod_id](bool ok, const std::string& archive,
                                  const std::string& error) {
        QMetaObject::invokeMethod(
            this,
            [this, mod_id, ok, archive, error]() {
                on_fetch_done(mod_id, ok, QString::fromStdString(archive),
                              QString::fromStdString(error));
            },
            Qt::QueuedConnection);
    };
    auto* thread = new FetchThread(
        mod_id, *engine_mod, std::filesystem::path(dest_dir.toStdString()),
        &fetch_cancel_, std::move(on_progress), std::move(on_meta),
        std::move(on_done), this);
    fetch_thread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (fetch_thread_ != thread) return;  // torn down in the dtor
        fetch_thread_ = nullptr;
        thread->deleteLater();
        pump_download_queue();
    });
    thread->start();
}

QString ModpackInstallWizard::step_title(Step step) {
    switch (step) {
        case Step::Intro: return tr("Intro");
        case Step::Paths: return tr("Paths");
        case Step::Choices: return tr("Choices");
        case Step::IniTweaks: return tr("INI Tweaks");
        case Step::Downloads: return tr("Downloads");
        case Step::RunTools: return tr("Run Tools");
        case Step::Patches: return tr("Patches");
        case Step::Finishing: return tr("Finishing");
        case Step::End: return tr("End");
    }
    return {};
}

void ModpackInstallWizard::build_steps() {
    const std::vector<Step> order = {
        Step::Intro,   Step::Paths,   Step::Choices, Step::IniTweaks,
        Step::Downloads, Step::RunTools, Step::Patches, Step::Finishing,
        Step::End,
    };
    for (Step id : order) {
        StepState state;
        state.id = id;
        state.title = step_title(id);
        // Append mode reuses the existing instance: Paths already exist.
        state.skipped = (id == Step::Paths && mode_ == Mode::Append);
        state.visited = state.skipped;
        steps_.push_back(state);
    }
    // First visible step counts as visited.
    for (auto& step : steps_) {
        if (!step.skipped) {
            step.visited = true;
            break;
        }
    }
}

void ModpackInstallWizard::build_pages() {
    stack_->addWidget(wrap_scroll(build_intro_page()));      // Intro
    stack_->addWidget(wrap_scroll(build_paths_page()));      // Paths
    stack_->addWidget(wrap_scroll(build_choices_page()));    // Choices
    stack_->addWidget(wrap_scroll(build_ini_page()));        // INI Tweaks
    stack_->addWidget(build_downloads_page());               // Downloads
    stack_->addWidget(wrap_scroll(build_run_tools_page()));  // Run Tools
    stack_->addWidget(build_patches_page());                 // Patches
    stack_->addWidget(wrap_scroll(build_finishing_page()));  // Finishing
    stack_->addWidget(wrap_scroll(build_end_page()));        // End
}

void ModpackInstallWizard::go_to(int index) {
    if (index < 0 || index >= static_cast<int>(steps_.size())) return;
    if (steps_[index].skipped) return;
    current_ = index;
    steps_[current_].visited = true;
    stack_->setCurrentIndex(current_);
    on_page_entered(current_);
    refresh_chrome();
}

void ModpackInstallWizard::on_next() {
    if (current_ >= static_cast<int>(steps_.size()) - 1) {
        accept();
        return;
    }
    // Gate: every exactly-one choice group needs a selection.
    if (steps_[current_].id == Step::Choices && !choices_complete()) {
        QMessageBox::information(
            this, tr("Choices"),
            tr("Please pick one mod for each required choice group."));
        return;
    }
    // Gate: required downloads must finish first.
    if (steps_[current_].id == Step::Downloads && !downloads_complete()) {
        QMessageBox::information(
            this, tr("Downloads"),
            tr("Required mods are still downloading. "
               "Wait for them to finish or fix failures first."));
        return;
    }
    int next = current_ + 1;
    while (next < static_cast<int>(steps_.size()) && steps_[next].skipped) {
        ++next;
    }
    if (next >= static_cast<int>(steps_.size())) {
        accept();
        return;
    }
    go_to(next);
}

void ModpackInstallWizard::on_back() {
    int prev = current_ - 1;
    while (prev >= 0 && steps_[prev].skipped) --prev;
    if (prev >= 0) go_to(prev);
}

void ModpackInstallWizard::on_cancel() {
    if (current_ > 0) {
        const auto answer = QMessageBox::question(
            this, tr("Install Modpack"),
            tr("Cancel the modpack install? Progress will be lost."));
        if (answer != QMessageBox::Yes) return;
    }
    reject();
}

void ModpackInstallWizard::on_step_clicked(QListWidgetItem* item) {
    const int index = sidebar_->row(item);
    // Only back-navigation to already-visited steps; never forward past
    // the furthest visited step and never onto skipped steps.
    int furthest = current_;
    for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
        if (steps_[i].visited && !steps_[i].skipped) furthest = i;
    }
    if (index <= furthest && !steps_[index].skipped) go_to(index);
    refresh_chrome();
}

void ModpackInstallWizard::refresh_chrome() {
    const QPalette palette = this->palette();
    const QColor dim = palette.color(QPalette::Disabled, QPalette::WindowText);
    for (int i = 0; i < static_cast<int>(steps_.size()); ++i) {
        auto* item = sidebar_->item(i);
        QString label = steps_[i].title;
        QFont font = item->font();
        if (steps_[i].skipped) {
            font.setStrikeOut(true);
            font.setBold(false);
            item->setForeground(dim);
        } else if (i == current_) {
            font.setBold(true);
            font.setStrikeOut(false);
            label = QStringLiteral("\u25CF ") + label;
        } else if (steps_[i].visited) {
            font.setBold(false);
            font.setStrikeOut(false);
            label = QStringLiteral("\u2713 ") + label;
        } else {
            font.setBold(false);
            font.setStrikeOut(false);
            label = QStringLiteral("\u25CB ") + label;
        }
        item->setFont(font);
        item->setText(label);
    }
    sidebar_->setCurrentRow(current_);

    // Back hidden on the first visible step; Next becomes Finish on End.
    int first = 0;
    while (first < static_cast<int>(steps_.size()) && steps_[first].skipped) {
        ++first;
    }
    back_button_->setVisible(current_ != first);
    const bool is_last =
        current_ == static_cast<int>(steps_.size()) - 1;
    next_button_->setText(is_last ? tr("Finish") : tr("Next >"));
    refresh_next_enabled();
}

void ModpackInstallWizard::refresh_next_enabled() {
    if (steps_[current_].id == Step::Downloads) {
        next_button_->setEnabled(downloads_complete());
    } else {
        next_button_->setEnabled(true);
    }
}

bool ModpackInstallWizard::choices_complete() const {
    for (const auto& group : pack_.manifest.choice_groups) {
        if (group.mode != "exactly-one") continue;
        const QString gid = QString::fromStdString(group.id);
        if (choice_selections_.value(gid).isEmpty()) return false;
    }
    return true;
}

bool ModpackInstallWizard::downloads_complete() const {
    for (const auto& mod : pack_.mods) {
        if (mod.category != ModCategory::Required) continue;
        const QString status =
            download_status_.value(QString::fromStdString(mod.id));
        if (status != QStringLiteral("downloaded")) return false;
    }
    return true;
}

QString ModpackInstallWizard::mod_display_name(const std::string& id) const {
    const ModEntry* mod = find_mod(pack_, id);
    if (mod != nullptr && !mod->name.empty())
        return QString::fromStdString(mod->name);
    return QString::fromStdString(id);
}

QString ModpackInstallWizard::instance_root() const {
    return instance_root_edit_ ? instance_root_edit_->text() : QString();
}

QString ModpackInstallWizard::mods_dir() const {
    return mods_dir_edit_ ? mods_dir_edit_->text() : QString();
}

QString ModpackInstallWizard::downloads_dir() const {
    return downloads_dir_edit_ ? downloads_dir_edit_->text() : QString();
}

QString ModpackInstallWizard::profile_path() const {
    return profile_path_edit_ ? profile_path_edit_->text() : QString();
}

// ---------------------------------------------------------------------------
// Page builders
// ---------------------------------------------------------------------------

QWidget* ModpackInstallWizard::build_intro_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    const auto& info = pack_.manifest.info;

    auto* name = new QLabel(
        QString::fromStdString(info.name.empty() ? pack_.manifest.id
                                                   : info.name),
        page);
    QFont title_font = name->font();
    title_font.setPointSize(title_font.pointSize() + 6);
    title_font.setBold(true);
    name->setFont(title_font);
    name->setWordWrap(true);
    layout->addWidget(name);

    if (!info.author.empty()) {
        layout->addWidget(new QLabel(tr("by %1").arg(
            QString::fromStdString(info.author)), page));
    }
    if (!info.description.empty()) {
        auto* desc = new QLabel(QString::fromStdString(info.description), page);
        desc->setWordWrap(true);
        layout->addWidget(desc);
    }
    if (!info.homepage.empty()) {
        auto* link = new QLabel(
            QStringLiteral("<a href=\"%1\">%1</a>")
                .arg(QString::fromStdString(info.homepage).toHtmlEscaped()),
            page);
        link->setOpenExternalLinks(true);
        link->setWordWrap(true);
        layout->addWidget(link);
    }

    QStringList dates;
    if (!info.created_at.empty())
        dates << tr("Created: %1").arg(
            QString::fromStdString(info.created_at));
    if (!info.updated_at.empty())
        dates << tr("Updated: %1").arg(
            QString::fromStdString(info.updated_at));
    if (!dates.isEmpty()) {
        auto* dates_label = new QLabel(dates.join(QStringLiteral("  |  ")),
                                       page);
        dates_label->setWordWrap(true);
        layout->addWidget(dates_label);
    }

    layout->addWidget(new QLabel(
        tr("%1 mods, revision %2")
            .arg(pack_.mods.size())
            .arg(pack_.manifest.revision),
        page));

    if (!pack_.manifest.tools.empty()) {
        auto* tools_box =
            new QGroupBox(tr("Required tools"), page);
        auto* tools_layout = new QVBoxLayout(tools_box);
        for (const auto& tool : pack_.manifest.tools) {
            QString entry = QString::fromStdString(
                tool.name.empty() ? tool.id : tool.name);
            if (!tool.homepage.empty()) {
                entry += QStringLiteral(" (<a href=\"%1\">%1</a>)")
                             .arg(QString::fromStdString(tool.homepage)
                                      .toHtmlEscaped());
            }
            auto* tool_label = new QLabel(entry, tools_box);
            tool_label->setOpenExternalLinks(true);
            tool_label->setWordWrap(true);
            tools_layout->addWidget(tool_label);
        }
        layout->addWidget(tools_box);
    }

    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_paths_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("Choose where the new instance and its directories live."), page));

    auto* form = new QFormLayout();
    instance_root_edit_ = new QLineEdit(page);
    mods_dir_edit_ = new QLineEdit(page);
    downloads_dir_edit_ = new QLineEdit(page);
    profile_path_edit_ = new QLineEdit(page);

    auto add_row = [&](const QString& label, QLineEdit* edit) {
        auto* row = new QWidget(page);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->addWidget(edit, 1);
        auto* browse = new QPushButton(tr("Browse..."), row);
        connect(browse, &QPushButton::clicked, this,
                [this, edit]() { on_browse_path(edit); });
        row_layout->addWidget(browse);
        form->addRow(label, row);
    };
    add_row(tr("Instance root:"), instance_root_edit_);
    add_row(tr("Mods directory:"), mods_dir_edit_);
    add_row(tr("Downloads directory:"), downloads_dir_edit_);
    add_row(tr("Profile path:"), profile_path_edit_);
    layout->addLayout(form);
    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_choices_page() {
    if (pack_.manifest.choice_groups.empty()) {
        return make_empty_note(tr("No choices to make."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    for (const auto& group : pack_.manifest.choice_groups) {
        const QString gid = QString::fromStdString(group.id);
        auto* box = new QGroupBox(
            QString::fromStdString(group.name.empty() ? group.id : group.name) +
                (group.mode == "exactly-one"
                     ? tr(" (pick one)")
                     : tr(" (pick at most one)")),
            page);
        auto* box_layout = new QVBoxLayout(box);
        const bool exactly_one = group.mode == "exactly-one";
        QButtonGroup* radio_group = nullptr;
        if (exactly_one) {
            radio_group = new QButtonGroup(box);
            radio_group->setExclusive(true);
        }
        for (const auto& member_id : group.member_mod_ids) {
            const QString mid = QString::fromStdString(member_id);
            const QString label = mod_display_name(member_id);
            const bool selected =
                choice_selections_.value(gid).contains(mid);
            if (exactly_one) {
                auto* radio = new QRadioButton(label, box);
                radio->setChecked(selected);
                radio->setProperty("group_id", gid);
                radio->setProperty("mod_id", mid);
                radio_group->addButton(radio);
                connect(radio, &QRadioButton::toggled, this,
                        &ModpackInstallWizard::on_choice_toggled);
                box_layout->addWidget(radio);
            } else {
                auto* check = new QCheckBox(label, box);
                check->setChecked(selected);
                check->setProperty("group_id", gid);
                check->setProperty("mod_id", mid);
                connect(check, &QCheckBox::toggled, this,
                        &ModpackInstallWizard::on_choice_toggled);
                box_layout->addWidget(check);
            }
        }
        layout->addWidget(box);
    }
    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_ini_page() {
    if (pack_.ini_edits.empty()) {
        return make_empty_note(tr("No INI tweaks to configure."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    for (const auto& entry : pack_.ini_edits) {
        auto* box = new QGroupBox(
            QString::fromStdString(entry.target_file), page);
        auto* box_layout = new QVBoxLayout(box);
        for (const auto& tweak : entry.tweaks) {
            const QString tid = QString::fromStdString(tweak.id);
            const bool required = tweak.status == "required";
            QString label = QString::fromStdString(tweak.name);
            label += required ? tr(" (required)") : tr(" (recommended)");
            if (tweak.has_source_mod_id && !tweak.source_mod_id.empty()) {
                label += tr(" [from %1]").arg(
                    mod_display_name(tweak.source_mod_id));
            }
            auto* check = new QCheckBox(label, box);
            check->setProperty("tweak_id", tid);
            if (required) {
                check->setChecked(true);
                check->setEnabled(false);
                ini_enabled_[tid] = true;
            } else {
                check->setChecked(ini_enabled_.value(tid, tweak.enabled));
                connect(check, &QCheckBox::toggled, this,
                        &ModpackInstallWizard::on_ini_toggled);
            }
            box_layout->addWidget(check);
        }
        layout->addWidget(box);
    }
    layout->addStretch(1);
    return page;
}

QWidget* ModpackInstallWizard::build_downloads_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    downloads_bar_ = new QProgressBar(page);
    downloads_bar_->setMinimum(0);
    downloads_bar_->setMaximum(100);
    layout->addWidget(downloads_bar_);

    auto* actions = new QHBoxLayout();
    downloads_start_all_ = new QPushButton(tr("Download All"), page);
    connect(downloads_start_all_, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_download_start_all);
    actions->addWidget(downloads_start_all_);
    actions->addStretch(1);
    layout->addLayout(actions);

    downloads_table_ = new QTableWidget(page);
    downloads_table_->setColumnCount(5);
    downloads_table_->setHorizontalHeaderLabels(
        {tr(""), tr("Mod"), tr("Source"), tr("Size"), tr("")});
    downloads_table_->horizontalHeader()->setStretchLastSection(true);
    downloads_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    downloads_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    downloads_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(downloads_table_, 1);

    downloads_hint_ = new QLabel(page);
    downloads_hint_->setWordWrap(true);
    layout->addWidget(downloads_hint_);

    refresh_downloads_ui();
    return page;
}

QWidget* ModpackInstallWizard::build_run_tools_page() {
    std::vector<const engine::gmmpack::ExecutableEntry*> setup;
    for (const auto& exe : pack_.executables) {
        if (exe.role == "setup" && exe.auto_run) setup.push_back(&exe);
    }
    if (setup.empty()) {
        return make_empty_note(tr("No setup tools to run."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    tools_status_ = new QLabel(page);
    tools_status_->setWordWrap(true);
    layout->addWidget(tools_status_);

    tools_table_ = new QTableWidget(page);
    tools_table_->setColumnCount(4);
    tools_table_->setHorizontalHeaderLabels(
        {tr("Tool"), tr("Source mod"), tr("Status"), tr("")});
    tools_table_->horizontalHeader()->setStretchLastSection(true);
    tools_table_->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    tools_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tools_table_->setRowCount(static_cast<int>(setup.size()));
    int row = 0;
    for (const auto* exe : setup) {
        const QString eid = QString::fromStdString(exe->id);
        tools_table_->setItem(
            row, 0, new QTableWidgetItem(QString::fromStdString(exe->id)));
        tools_table_->setItem(row, 1,
                              new QTableWidgetItem(mod_display_name(
                                  exe->source_mod_id)));
        tools_table_->setItem(
            row, 2, new QTableWidgetItem(tool_status_.value(eid)));
        tools_table_->item(row, 0)->setData(Qt::UserRole, eid);
        auto* run = new QPushButton(tr("Run"), tools_table_);
        run->setProperty("exe_id", eid);
        connect(run, &QPushButton::clicked, this,
                &ModpackInstallWizard::on_run_tool_one);
        tools_table_->setCellWidget(row, 3, run);
        ++row;
    }
    layout->addWidget(tools_table_, 1);

    // Surface output-capture and argument details below the table.
    QStringList notes;
    for (const auto* exe : setup) {
        QString note = QString::fromStdString(exe->id) + QStringLiteral(": ") +
                       QString::fromStdString(exe->relative_path);
        if (!exe->arguments.empty()) {
            QStringList args;
            for (const auto& arg : exe->arguments)
                args << QString::fromStdString(arg);
            note += QStringLiteral(" ") + args.join(QStringLiteral(" "));
        }
        if (exe->output && exe->output->capture == "syntheticMod") {
            note += tr(" (output captured as mod %1)")
                        .arg(QString::fromStdString(
                            exe->output->synthetic_mod_id));
        }
        notes << note;
    }
    auto* detail = new QLabel(notes.join(QStringLiteral("\n")), page);
    detail->setWordWrap(true);
    layout->addWidget(detail);
    tools_status_->setText(tr("Setup tools are run in pack order."));
    return page;
}

QWidget* ModpackInstallWizard::build_patches_page() {
    if (patch_chains_.empty()) {
        return make_empty_note(tr("No binary patches."));
    }
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(
        tr("These patches modify mod files. Allow only patches you trust."),
        page));
    if (!patch_plan_error_.isEmpty()) {
        auto* warning = new QLabel(
            tr("Patch validation reported errors:\n%1")
                .arg(patch_plan_error_),
            page);
        warning->setWordWrap(true);
        layout->addWidget(warning);
    }

    patches_table_ = new QTableWidget(page);
    patches_table_->setColumnCount(4);
    patches_table_->setHorizontalHeaderLabels(
        {tr("Mod"), tr("Target file"), tr("Steps"), tr("Status")});
    patches_table_->horizontalHeader()->setStretchLastSection(true);
    patches_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    patches_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    patches_table_->setRowCount(static_cast<int>(patch_chains_.size()));
    int row = 0;
    for (const auto& chain : patch_chains_) {
        const QString key = QString::fromStdString(chain.mod_id);
        QStringList targets;
        for (const auto& patch : chain.patches) {
            const QString target =
                QString::fromStdString(patch.target_path);
            if (!targets.contains(target)) targets << target;
        }
        patches_table_->setItem(
            row, 0, new QTableWidgetItem(mod_display_name(chain.mod_id)));
        patches_table_->setItem(
            row, 1,
            new QTableWidgetItem(targets.join(QStringLiteral(", "))));
        patches_table_->setItem(
            row, 2,
            new QTableWidgetItem(tr("%n step(s)", nullptr,
                                    static_cast<int>(chain.patches.size()))));
        auto* status = new QTableWidgetItem(
            patch_allowed_.value(key) ? tr("Allow") : tr("Deny"));
        status->setData(Qt::UserRole, key);
        patches_table_->setItem(row, 3, status);
        ++row;
    }
    connect(patches_table_, &QTableWidget::cellChanged, this,
            &ModpackInstallWizard::on_patch_cell_changed);
    connect(patches_table_, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column != 3 || patches_table_ == nullptr) return;
                auto* item = patches_table_->item(row, column);
                if (item == nullptr) return;
                const QString key = item->data(Qt::UserRole).toString();
                const bool allowed = !patch_allowed_.value(key, false);
                patch_allowed_[key] = allowed;
                const bool blocked = patches_table_->blockSignals(true);
                item->setText(allowed ? tr("Allow") : tr("Deny"));
                patches_table_->blockSignals(blocked);
            });
    layout->addWidget(patches_table_, 1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* allow_all = new QPushButton(tr("Allow All"), page);
    auto* deny_all = new QPushButton(tr("Deny All"), page);
    connect(allow_all, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_patch_allow_all);
    connect(deny_all, &QPushButton::clicked, this,
            &ModpackInstallWizard::on_patch_deny_all);
    buttons->addWidget(allow_all);
    buttons->addWidget(deny_all);
    layout->addLayout(buttons);
    return page;
}

QWidget* ModpackInstallWizard::build_finishing_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(tr("Applying the modpack..."), page));

    finishing_table_ = new QTableWidget(page);
    finishing_table_->setColumnCount(2);
    finishing_table_->setHorizontalHeaderLabels({tr(""), tr("Action")});
    finishing_table_->horizontalHeader()->setStretchLastSection(true);
    finishing_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    finishing_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QStringList actions = {
        tr("Creating separators from tree structure"),
        tr("Parenting mods (nested separators)"),
        tr("Applying load order from tree"),
        tr("Platform setup (proton, Steam overlay, prefix files)"),
    };
    finishing_table_->setRowCount(actions.size());
    for (int i = 0; i < actions.size(); ++i) {
        finishing_table_->setItem(i, 0, new QTableWidgetItem(QStringLiteral("\u25CB")));
        finishing_table_->setItem(i, 1, new QTableWidgetItem(actions[i]));
        finishing_done_[i] = false;
    }
    layout->addWidget(finishing_table_, 1);
    return page;
}

QWidget* ModpackInstallWizard::build_end_page() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    auto* ready = new QLabel(tr("Your modpack is ready!"), page);
    QFont title_font = ready->font();
    title_font.setPointSize(title_font.pointSize() + 6);
    title_font.setBold(true);
    ready->setFont(title_font);
    ready->setAlignment(Qt::AlignCenter);
    layout->addWidget(ready);

    QString closing;
    if (pack_.instructions && !pack_.instructions->empty()) {
        closing = QString::fromStdString(*pack_.instructions);
    }
    if (closing.isEmpty()) {
        closing = tr("Thanks for installing %1.")
                      .arg(QString::fromStdString(
                          pack_.manifest.info.name.empty()
                              ? pack_.manifest.id
                              : pack_.manifest.info.name));
    }
    auto* notes = new QLabel(closing, page);
    notes->setWordWrap(true);
    notes->setAlignment(Qt::AlignCenter);
    layout->addWidget(notes);
    layout->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------------
// Page hooks + slots
// ---------------------------------------------------------------------------

void ModpackInstallWizard::on_page_entered(int index) {
    if (steps_[index].id == Step::Downloads) {
        refresh_downloads_ui();
    } else if (steps_[index].id == Step::Finishing) {
        start_finishing_animation();
    }
}

void ModpackInstallWizard::refresh_downloads_ui() {
    if (downloads_table_ == nullptr) return;
    // Phase-ordered: phase 0 first, then 1, ...
    const std::vector<QString> ids = ordered_download_ids();

    downloads_table_->setRowCount(static_cast<int>(ids.size()));
    int done = 0;
    int row = 0;
    for (const QString& mid : ids) {
        const ModEntry* mod = find_mod(pack_, mid.toStdString());
        if (mod == nullptr) continue;
        const QString status =
            download_status_.value(mid, QStringLiteral("pending"));
        if (status == QStringLiteral("downloaded")) ++done;
        const DownloadRoute route = route_for(*mod);

        QString label = QStringLiteral("%1 %2").arg(
            status_icon(status),
            QString::fromStdString(
                mod->name.empty() ? mod->id : mod->name));
        if (status == QStringLiteral("downloading") &&
            download_fraction_.contains(mid)) {
            const int pct =
                static_cast<int>(download_fraction_.value(mid) * 100.0);
            label += tr(" (%1%)").arg(pct);
        }
        downloads_table_->setItem(row, 0, new QTableWidgetItem(
            QStringLiteral("phase %1").arg(mod->phase)));
        auto* name_item = new QTableWidgetItem(label);
        name_item->setData(Qt::UserRole, mid);
        QString tip = category_name(mod->category);
        if (!route.reason.isEmpty()) tip += QStringLiteral(" - ") + route.reason;
        if (status == QStringLiteral("failed") &&
            download_error_.contains(mid)) {
            tip += QStringLiteral("\n") + download_error_.value(mid);
        }
        name_item->setToolTip(tip);
        downloads_table_->setItem(row, 1, name_item);
        downloads_table_->setItem(
            row, 2,
            new QTableWidgetItem(source_provider(*mod) + QStringLiteral(" - ") +
                                 source_detail(*mod)));
        downloads_table_->setItem(
            row, 3, new QTableWidgetItem(format_size(source_file_size(*mod))));

        QWidget* cell = nullptr;
        const bool is_optional = mod->category == ModCategory::Optional;
        if (status == QStringLiteral("downloading")) {
            cell = new QLabel(status_icon(status) + tr(" active..."),
                              downloads_table_);
        } else if (status == QStringLiteral("downloaded") ||
                   status == QStringLiteral("skipped")) {
            cell = new QLabel(status_icon(status), downloads_table_);
        } else if (route.path == Collection::DownloadPath::ExternalClient) {
            // Steam Workshop: the subscription lives outside GMM.
            auto* box = new QWidget(downloads_table_);
            auto* box_layout = new QHBoxLayout(box);
            box_layout->setContentsMargins(0, 0, 0, 0);
            auto* note = new QLabel(tr("Steam client"), box);
            note->setToolTip(route.reason);
            box_layout->addWidget(note);
            auto* mark = new QPushButton(tr("Mark done"), box);
            mark->setProperty("mod_id", mid);
            connect(mark, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_mark_done);
            box_layout->addWidget(mark);
            if (is_optional) {
                auto* skip = new QPushButton(tr("Skip"), box);
                skip->setProperty("mod_id", mid);
                connect(skip, &QPushButton::clicked, this,
                        &ModpackInstallWizard::on_download_skip_optional);
                box_layout->addWidget(skip);
            }
            cell = box;
        } else if (route.path == Collection::DownloadPath::Browser) {
            auto* box = new QWidget(downloads_table_);
            auto* box_layout = new QHBoxLayout(box);
            box_layout->setContentsMargins(0, 0, 0, 0);
            if (!route.open_url.isEmpty()) {
                auto* open = new QPushButton(tr("Open"), box);
                open->setProperty("mod_id", mid);
                open->setToolTip(route.reason);
                connect(open, &QPushButton::clicked, this,
                        &ModpackInstallWizard::on_download_open_browser);
                box_layout->addWidget(open);
            }
            auto* mark = new QPushButton(tr("Mark done"), box);
            mark->setToolTip(tr("Fetch the file in the browser, then mark it done."));
            mark->setProperty("mod_id", mid);
            connect(mark, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_mark_done);
            box_layout->addWidget(mark);
            if (is_optional) {
                auto* skip = new QPushButton(tr("Skip"), box);
                skip->setProperty("mod_id", mid);
                connect(skip, &QPushButton::clicked, this,
                        &ModpackInstallWizard::on_download_skip_optional);
                box_layout->addWidget(skip);
            }
            cell = box;
        } else if (is_optional) {
            auto* box = new QWidget(downloads_table_);
            auto* box_layout = new QHBoxLayout(box);
            box_layout->setContentsMargins(0, 0, 0, 0);
            auto* dl = new QPushButton(
                status == QStringLiteral("failed") ? tr("Retry") : tr("Get"),
                box);
            dl->setProperty("mod_id", mid);
            connect(dl, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_one);
            box_layout->addWidget(dl);
            auto* skip = new QPushButton(tr("Skip"), box);
            skip->setProperty("mod_id", mid);
            connect(skip, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_skip_optional);
            box_layout->addWidget(skip);
            cell = box;
        } else {
            auto* dl = new QPushButton(
                status == QStringLiteral("failed") ? tr("Retry") : tr("Get"),
                downloads_table_);
            dl->setProperty("mod_id", mid);
            connect(dl, &QPushButton::clicked, this,
                    &ModpackInstallWizard::on_download_one);
            cell = dl;
        }
        downloads_table_->setCellWidget(row, 4, cell);
        ++row;
    }

    const int total = static_cast<int>(ids.size());
    downloads_bar_->setMaximum(total == 0 ? 1 : total);
    downloads_bar_->setValue(done);
    if (!active_download_id_.isEmpty() &&
        download_fraction_.contains(active_download_id_)) {
        const int pct = static_cast<int>(
            download_fraction_.value(active_download_id_) * 100.0);
        downloads_bar_->setFormat(
            tr("%1 of %2 downloaded - %3 (%4%)")
                .arg(done)
                .arg(total)
                .arg(mod_display_name(active_download_id_.toStdString()))
                .arg(pct));
    } else {
        downloads_bar_->setFormat(tr("%1 of %2 downloaded").arg(done).arg(total));
    }
    if (downloads_start_all_ != nullptr) {
        downloads_start_all_->setEnabled(fetch_thread_ == nullptr &&
                                         !downloads_complete());
    }
    if (downloads_hint_ != nullptr) {
        const Collection::AccountStatus nexus =
            Collection::Nexus::account_status();
        const QString mode = nexus.can_auto_download
                                 ? tr("Nexus API auto-download (premium)")
                                 : tr("Nexus via browser (free/anonymous)");
        downloads_hint_->setText(
            tr("%1. Downloads run in phase order, one at a time. "
               "Next unlocks once all required mods finish.").arg(mode));
    }
    refresh_next_enabled();
}

void ModpackInstallWizard::on_browse_path(QLineEdit* edit) {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose directory"), edit->text());
    if (!dir.isEmpty()) edit->setText(dir);
}

void ModpackInstallWizard::on_choice_toggled() {
    QObject* sender_obj = sender();
    if (sender_obj == nullptr) return;
    const QString gid = sender_obj->property("group_id").toString();
    const QString mid = sender_obj->property("mod_id").toString();
    const auto* radio = qobject_cast<const QRadioButton*>(sender_obj);
    if (radio != nullptr) {
        // exactly-one: radio exclusivity already enforces one selection.
        if (radio->isChecked()) choice_selections_[gid] = QStringList(mid);
        return;
    }
    // at-most-one: keep zero or one checked.
    const auto* check = qobject_cast<const QCheckBox*>(sender_obj);
    if (check == nullptr) return;
    QStringList selected = choice_selections_.value(gid);
    selected.removeAll(mid);
    if (check->isChecked()) {
        selected.clear();  // at most one: new pick replaces the old one.
        selected << mid;
        // Uncheck the sibling boxes without recursing through signals.
        auto* page = stack_->widget(static_cast<int>(Step::Choices));
        const auto boxes = page->findChildren<QCheckBox*>();
        for (auto* sibling : boxes) {
            if (sibling != check &&
                sibling->property("group_id").toString() == gid) {
                const bool blocked = sibling->blockSignals(true);
                sibling->setChecked(false);
                sibling->blockSignals(blocked);
            }
        }
    }
    choice_selections_[gid] = selected;
}

void ModpackInstallWizard::on_ini_toggled() {
    const auto* check = qobject_cast<const QCheckBox*>(sender());
    if (check == nullptr) return;
    ini_enabled_[check->property("tweak_id").toString()] = check->isChecked();
}

void ModpackInstallWizard::on_download_one() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    const QString mid = button->property("mod_id").toString();
    const ModEntry* mod = find_mod(pack_, mid.toStdString());
    if (mod == nullptr) return;
    // Browser/external rows never fetch: Get opens the page instead.
    if (route_for(*mod).path != Collection::DownloadPath::Auto) {
        on_download_open_browser();
        return;
    }
    download_status_[mid] = QStringLiteral("pending");
    download_error_.remove(mid);
    // Front-of-queue: a manual Get jumps ahead of the batch.
    download_queue_.push_front(mid);
    pump_download_queue();
}

void ModpackInstallWizard::on_download_start_all() {
    for (const QString& mid : ordered_download_ids()) {
        const QString status = download_status_.value(mid);
        if (status == QStringLiteral("pending") ||
            status == QStringLiteral("failed")) {
            download_queue_.push_back(mid);
        }
    }
    pump_download_queue();
}

void ModpackInstallWizard::on_download_open_browser() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    const QString mid = button->property("mod_id").toString();
    const ModEntry* mod = find_mod(pack_, mid.toStdString());
    if (mod == nullptr) return;
    const QString url = route_for(*mod).open_url;
    if (url.isEmpty()) return;
    QDesktopServices::openUrl(QUrl(url));
}

void ModpackInstallWizard::on_download_mark_done() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    const QString mid = button->property("mod_id").toString();
    download_status_[mid] = QStringLiteral("downloaded");
    download_error_.remove(mid);
    refresh_downloads_ui();
}

void ModpackInstallWizard::on_fetch_progress(const QString& mod_id,
                                             int64_t downloaded,
                                             int64_t total) {
    if (total > 0) {
        download_fraction_[mod_id] =
            static_cast<double>(downloaded) / static_cast<double>(total);
    }
    refresh_downloads_ui();
}

void ModpackInstallWizard::on_fetch_meta(const QString& mod_id,
                                         const QString& archive_name,
                                         const QString& display_name) {
    Q_UNUSED(archive_name);
    if (!display_name.isEmpty()) {
        // Surface the provider-resolved name live (mirrors the main-window
        // download_meta flow). The pack entry itself is left untouched.
        for (int row = 0; row < downloads_table_->rowCount(); ++row) {
            auto* item = downloads_table_->item(row, 1);
            if (item != nullptr &&
                item->data(Qt::UserRole).toString() == mod_id) {
                const QString label = QStringLiteral("%1 %2").arg(
                    status_icon(QStringLiteral("downloading")), display_name);
                item->setText(label);
                break;
            }
        }
    }
}

void ModpackInstallWizard::on_fetch_done(const QString& mod_id, bool ok,
                                         const QString& archive_path,
                                         const QString& error) {
    active_download_id_.clear();
    download_fraction_.remove(mod_id);
    if (ok) {
        download_status_[mod_id] = QStringLiteral("downloaded");
        download_error_.remove(mod_id);
        if (!archive_path.isEmpty())
            download_archive_[mod_id] = archive_path;
        engine::Logger::instance().debug(
            "[Modpack] download complete: " + mod_id.toStdString() + " -> " +
            archive_path.toStdString());
    } else {
        download_status_[mod_id] = QStringLiteral("failed");
        download_error_[mod_id] =
            error.isEmpty() ? tr("download failed") : error;
        engine::Logger::instance().warn(
            "[Modpack] download failed: " + mod_id.toStdString() + ": " +
            download_error_[mod_id].toStdString());
    }
    refresh_downloads_ui();
    // The finished-thread handler pumps the queue; this only covers the
    // case where the fetch ran while no thread was tracked.
    if (fetch_thread_ == nullptr) pump_download_queue();
}

void ModpackInstallWizard::on_download_skip_optional() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    download_status_[button->property("mod_id").toString()] =
        QStringLiteral("skipped");
    refresh_downloads_ui();
}

void ModpackInstallWizard::on_run_tool_one() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    tool_running_id_ = button->property("exe_id").toString();
    tool_status_[tool_running_id_] = QStringLiteral("running");
    tool_sim_ticks_ = 0;
    if (tools_status_ != nullptr) {
        tools_status_->setText(tr("Running %1...").arg(tool_running_id_));
    }
    disconnect(sim_timer_, nullptr, nullptr, nullptr);
    connect(sim_timer_, &QTimer::timeout, this,
            &ModpackInstallWizard::on_tool_sim_tick);
    sim_timer_->start(150);
}

void ModpackInstallWizard::on_tool_sim_tick() {
    if (++tool_sim_ticks_ < 3) return;
    sim_timer_->stop();
    tool_status_[tool_running_id_] = QStringLiteral("done");
    if (tools_status_ != nullptr) {
        tools_status_->setText(tr("%1 finished.").arg(tool_running_id_));
    }
    // Refresh the status column in place.
    if (tools_table_ != nullptr) {
        for (int row = 0; row < tools_table_->rowCount(); ++row) {
            auto* id_item = tools_table_->item(row, 0);
            if (id_item != nullptr &&
                id_item->data(Qt::UserRole).toString() == tool_running_id_) {
                tools_table_->setItem(
                    row, 2, new QTableWidgetItem(QStringLiteral("done")));
                auto* skip = new QPushButton(tr("Skip"), tools_table_);
                skip->setProperty("exe_id", tool_running_id_);
                connect(skip, &QPushButton::clicked, this,
                        &ModpackInstallWizard::on_run_tool_skip);
                tools_table_->setCellWidget(row, 3, skip);
            }
        }
    }
}

void ModpackInstallWizard::on_run_tool_skip() {
    const auto* button = qobject_cast<const QPushButton*>(sender());
    if (button == nullptr) return;
    const QString eid = button->property("exe_id").toString();
    tool_status_[eid] = QStringLiteral("skipped");
    if (tools_table_ != nullptr) {
        for (int row = 0; row < tools_table_->rowCount(); ++row) {
            auto* id_item = tools_table_->item(row, 0);
            if (id_item != nullptr &&
                id_item->data(Qt::UserRole).toString() == eid) {
                tools_table_->setItem(
                    row, 2, new QTableWidgetItem(QStringLiteral("skipped")));
            }
        }
    }
}

void ModpackInstallWizard::on_patch_allow_all() {
    for (auto it = patch_allowed_.begin(); it != patch_allowed_.end(); ++it) {
        it.value() = true;
    }
    if (patches_table_ != nullptr) {
        const bool blocked = patches_table_->blockSignals(true);
        for (int row = 0; row < patches_table_->rowCount(); ++row) {
            patches_table_->item(row, 3)->setText(tr("Allow"));
        }
        patches_table_->blockSignals(blocked);
    }
}

void ModpackInstallWizard::on_patch_deny_all() {
    for (auto it = patch_allowed_.begin(); it != patch_allowed_.end(); ++it) {
        it.value() = false;
    }
    if (patches_table_ != nullptr) {
        const bool blocked = patches_table_->blockSignals(true);
        for (int row = 0; row < patches_table_->rowCount(); ++row) {
            patches_table_->item(row, 3)->setText(tr("Deny"));
        }
        patches_table_->blockSignals(blocked);
    }
}

void ModpackInstallWizard::on_patch_cell_changed(int row, int column) {
    if (patches_table_ == nullptr || column != 3) return;
    auto* item = patches_table_->item(row, column);
    if (item == nullptr) return;
    // Status cell cycles Allow/Deny on double-click edit; single click on
    // the row toggles consent directly.
    const QString key = item->data(Qt::UserRole).toString();
    const bool allowed = item->text().compare(tr("Allow"), Qt::CaseInsensitive) == 0;
    patch_allowed_[key] = allowed;
}

void ModpackInstallWizard::start_finishing_animation() {
    if (finishing_table_ == nullptr || finishing_started_) return;
    finishing_started_ = true;
    finishing_tick_ = 0;
    disconnect(sim_timer_, nullptr, nullptr, nullptr);
    connect(sim_timer_, &QTimer::timeout, this,
            &ModpackInstallWizard::on_finishing_tick);
    sim_timer_->start(250);
}

void ModpackInstallWizard::on_finishing_tick() {
    if (finishing_table_ == nullptr) {
        sim_timer_->stop();
        return;
    }
    if (finishing_tick_ >= finishing_table_->rowCount()) {
        sim_timer_->stop();
        return;
    }
    finishing_table_->setItem(
        finishing_tick_, 0, new QTableWidgetItem(QStringLiteral("\u2713")));
    finishing_done_[finishing_tick_] = true;
    ++finishing_tick_;
    if (finishing_tick_ >= finishing_table_->rowCount()) sim_timer_->stop();
}

}  // namespace ui
