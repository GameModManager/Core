#include "ui/modinfo/source_tab.h"

#include "engine/mod/meta/mod_meta.h"
#include "engine/source/source_provider.h"
#include "ui/modinfo/mod_info_data.h"
#include "ui/modinfo/source_panels/generic_source_panel.h"
#include "ui/modinfo/source_panels/loverslab_source_panel.h"
#include "ui/modinfo/source_panels/nexus_source_panel.h"
#include "ui/modinfo/source_panels/source_info_panel.h"
#include "ui/modinfo/source_panels/steam_source_panel.h"
#include "ui/theme/icon_manager.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cctype>
#include <optional>
#include <string>

namespace ui {

namespace {

// Match a provider by either its source_type() ("nexus") or display_name()
// ("Nexus Mods"), case-insensitive. Returns nullptr when no provider in the
// SourceRegistry matches - the caller is then expected to fall back to a
// generic or placeholder panel.
engine::SourceProvider *find_provider(const QString &name) {
  std::string low = name.trimmed().toStdString();
  for (auto &c : low)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto *provider : engine::SourceRegistry::instance().providers()) {
    auto matches = [&low](const std::string &s) {
      std::string sl = s;
      for (auto &c : sl)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return sl == low;
    };
    if (matches(provider->source_type()) || matches(provider->display_name()))
      return provider;
  }
  return nullptr;
}

// Add a tab to sources_ with the vendor icon when one resolves. Used for
// the single source panel and for the "+" affordance.
void add_tab_with_icon(QTabWidget *tabs, QWidget *page, const QString &title,
                       const QString &source_key) {
  const std::string vendor_key =
      engine::vendor_icon_key(source_key.toStdString());
  if (vendor_key.empty()) {
    tabs->addTab(page, title);
  } else {
    tabs->addTab(
        page,
        engine::IconManager::instance().resolve_icon(
            QString::fromStdString(vendor_key)),
        title);
  }
}

// Determine the mod's actual source from meta + ModInfoData fallback.
// Returns the canonical source_type ("nexus", "loverslab", "steam") or an
// empty QString for manual / unknown. Strategy (Workspace-fqf5):
//   1. Use meta's [GameModManager]source_type when it names a known
//      provider. Empty / "manual" / unknown are NOT a source.
//   2. If absent, look at provider-specific sections already present in the
//      meta. A mod with [Nexusmods]/[LoversLab]/[SteamWorkshop] carrying
//      data shows that source even when the game's download_sources hook
//      does not declare the provider.
//   3. Fall back to the in-memory data_.source_type for mods whose meta
//      has no provider section yet (e.g. a brand-new install before
//      load_meta_for_mods has been called).
QString resolve_actual_source(const ModInfoData &data) {
  auto lower = [](QString s) { return s.toLower(); };
  if (data.load_meta) {
    auto meta = data.load_meta();
    const QString t = lower(QString::fromStdString(meta.source_type()));
    if (t == QLatin1String("nexus") || t == QLatin1String("loverslab") ||
        t == QLatin1String("steam")) {
      return t;
    }
    // No declared source_type, but a provider section may exist. Prefer
    // the section with the strongest signal (an actual id stored in it).
    if (meta.has_section("Nexusmods")) {
      const QString modid = QString::fromStdString(
          meta.get("Nexusmods", "modid"));
      if (!modid.isEmpty() && modid != QLatin1String("0") &&
          modid.toLongLong() > 0)
        return QStringLiteral("nexus");
    }
    if (meta.has_section("LoversLab")) {
      const QString fid =
          QString::fromStdString(meta.get("LoversLab", "fileid"));
      if (!fid.isEmpty() && fid.toLongLong() > 0)
        return QStringLiteral("loverslab");
    }
    if (meta.has_section("SteamWorkshop")) {
      const QString wid = QString::fromStdString(
          meta.get("SteamWorkshop", "workshop_id"));
      if (!wid.isEmpty() && wid.toLongLong() > 0)
        return QStringLiteral("steam");
    }
  }
  // Fall back to the controller-supplied data_.source_type for mods that
  // have no sidecar yet (a manual install before any load_meta round trip).
  const QString dt = lower(data.source_type);
  if (dt == QLatin1String("nexus") || dt == QLatin1String("loverslab") ||
      dt == QLatin1String("steam"))
    return dt;
  return {};
}

// Build a panel for the given source_type, using the typed SourceInfoPanel
// subclass when one exists and a GenericSourcePanel otherwise. The single
// tab the user sees - the rest of the Source tab is the "+" affordance.
QWidget *build_panel_for(const QString &source_type,
                         const ModInfoData &data, QWidget *parent) {
  if (source_type == QLatin1String("nexus")) {
    return new NexusSourcePanel(data, parent);
  }
  if (source_type == QLatin1String("loverslab")) {
    return new LoversLabSourcePanel(data, parent);
  }
  if (source_type == QLatin1String("steam")) {
    return new SteamSourcePanel(data, parent);
  }
  // Unknown / manual: try a registered generic provider that matches the
  // actual source_type string (some plugins use their own keys).
  if (auto *provider = find_provider(source_type)) {
    return new GenericSourcePanel(data, provider, parent);
  }
  return nullptr;
}

// Resolve a human-friendly tab title + vendor icon key for a source_type.
// Returns std::nullopt when there is no real provider to show and the
// caller should render a Manual placeholder instead.
struct SourceDisplay {
  QString title;
  QString icon_key;
};
std::optional<SourceDisplay> display_for_source(const QString &source_type) {
  for (auto *provider : engine::SourceRegistry::instance().providers()) {
    const QString pt = QString::fromStdString(provider->source_type())
                           .toLower();
    if (pt == QLatin1String("steamworkshop")) {
      if (source_type == QLatin1String("steam")) {
        return SourceDisplay{QString::fromStdString(provider->display_name()),
                             QStringLiteral("steam")};
      }
      continue;
    }
    if (pt == source_type) {
      return SourceDisplay{QString::fromStdString(provider->display_name()),
                           source_type};
    }
  }
  return std::nullopt;
}

// -- Add-source dialog -----------------------------------------------------

// A small modal dialog that lets the user pick a provider (Nexus / LoversLab
// / Steam / anything else in SourceRegistry) and supply the per-provider
// identifier(s). On accept, writes the provider section + canonical source
// keys to meta via the ModInfoData lambdas.
//
// We keep the dialog deliberately minimal: a provider combo, a small form
// with the fields each known provider needs, and OK / Cancel. The visible
// form changes when the combo selection changes.
class AddSourceDialog : public QDialog {
public:
  AddSourceDialog(const ModInfoData &data, QWidget *parent)
      : QDialog(parent), data_(data) {
    setWindowTitle(tr("Add Source"));
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr(
        "Attach this mod to a download source. The selected provider's "
        "metadata will be written to the mod's sidecar and the Source tab "
        "will reload with the new source."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout();
    provider_combo_ = new QComboBox(this);
    int nexus_index = -1;
    int loverslab_index = -1;
    int steam_index = -1;
    for (auto *provider : engine::SourceRegistry::instance().providers()) {
      provider_combo_->addItem(
          QString::fromStdString(provider->display_name()));
      const QString pt =
          QString::fromStdString(provider->source_type()).toLower();
      if (pt == QLatin1String("nexus"))
        nexus_index = provider_combo_->count() - 1;
      else if (pt == QLatin1String("loverslab"))
        loverslab_index = provider_combo_->count() - 1;
      else if (pt == QLatin1String("steam") ||
               pt == QLatin1String("steamworkshop"))
        steam_index = provider_combo_->count() - 1;
    }
    // Re-order so the three well-known providers come first. Anything else
    // (custom plugins) appears after.
    auto push_to_front = [this](int from_index) {
      if (from_index <= 0) return;
      const QString text = provider_combo_->itemText(from_index);
      provider_combo_->removeItem(from_index);
      provider_combo_->insertItem(0, text);
      provider_combo_->setCurrentIndex(0);
    };
    // Push the three known providers to the front in priority order:
    // Steam, LoversLab, Nexus. The most commonly re-attached sources are
    // first so the user does not have to scroll.
    if (steam_index >= 0) {
      push_to_front(steam_index);
      loverslab_index++;
      nexus_index++;
    }
    if (loverslab_index >= 0) {
      push_to_front(loverslab_index);
      nexus_index++;
    }
    if (nexus_index >= 0) {
      push_to_front(nexus_index);
    }
    form->addRow(tr("Provider:"), provider_combo_);
    layout->addLayout(form);

    // The fields stack swaps based on the chosen provider. Each provider
    // contributes a small QWidget built lazily and added to the stack; we
    // rebuild on combo change so edits do not silently carry over.
    field_stack_ = new QStackedWidget(this);
    layout->addWidget(field_stack_, 1);

    nexus_page_ = build_nexus_page();
    loverslab_page_ = build_loverslab_page();
    steam_page_ = build_steam_page();
    field_stack_->addWidget(nexus_page_);
    field_stack_->addWidget(loverslab_page_);
    field_stack_->addWidget(steam_page_);
    // Map combo index -> field page. Unknown providers (custom plugins)
    // get an empty page with an "edit in meta.ini" hint.
    unknown_page_ = new QLabel(tr(
        "This provider has no editable fields here. After confirming, the "
        "mod's source_type will be set and you can finish configuration by "
        "editing the meta sidecar directly."),
        this);
    unknown_page_->setWordWrap(true);
    field_stack_->addWidget(unknown_page_);

    connect(provider_combo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &AddSourceDialog::on_provider_changed);
    on_provider_changed(provider_combo_->currentIndex());

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this,
            &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this,
            &QDialog::reject);

    // OK is disabled until at least the minimum required field is filled
    // (the current provider's id field). Refresh on every edit and on
    // provider change.
    connect(nexus_mod_id_, &QLineEdit::textChanged, this,
            &AddSourceDialog::refresh_accept_enabled);
    connect(loverslab_fileid_, &QLineEdit::textChanged, this,
            &AddSourceDialog::refresh_accept_enabled);
    connect(steam_workshop_id_, &QLineEdit::textChanged, this,
            &AddSourceDialog::refresh_accept_enabled);
    refresh_accept_enabled();
  }

  // The provider that the user picked, in the canonical short form used
  // for [GameModManager]source_type ("nexus" / "loverslab" / "steam" / ...).
  QString chosen_source_type() const {
    const int idx = provider_combo_->currentIndex();
    if (idx < 0)
      return {};
    return provider_index_to_canonical(idx);
  }

  // Identifier for the chosen provider. For Nexus this is the mod id; for
  // LoversLab it is the file id; for Steam it is the workshop id. Custom
  // providers always get an empty id and rely on the user editing meta.ini.
  QString chosen_source_id() const {
    const QString t = chosen_source_type();
    if (t == QLatin1String("nexus"))
      return nexus_mod_id_->text().trimmed();
    if (t == QLatin1String("loverslab"))
      return loverslab_fileid_->text().trimmed();
    if (t == QLatin1String("steam"))
      return steam_workshop_id_->text().trimmed();
    return {};
  }

  // Per-provider secondary fields. May be empty when the user did not
  // enter them (LoversLab page_url, Steam none).
  QString loverslab_page_url() const {
    return loverslab_page_url_->text().trimmed();
  }

private:
  QWidget *build_nexus_page() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    nexus_mod_id_ = new QLineEdit(page);
    nexus_mod_id_->setPlaceholderText(QStringLiteral("e.g. 12345"));
    form->addRow(tr("Mod ID:"), nexus_mod_id_);
    auto *hint = new QLabel(tr(
        "The numeric mod id from the mod's Nexus URL. "
        "https://www.nexusmods.com/<game>/mods/<id>."),
        page);
    hint->setWordWrap(true);
    form->addRow(hint);
    return page;
  }
  QWidget *build_loverslab_page() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    loverslab_fileid_ = new QLineEdit(page);
    loverslab_fileid_->setPlaceholderText(QStringLiteral("e.g. 12345"));
    form->addRow(tr("File ID:"), loverslab_fileid_);
    loverslab_page_url_ = new QLineEdit(page);
    loverslab_page_url_->setPlaceholderText(
        QStringLiteral("https://www.loverslab.com/files/file/12345/"));
    form->addRow(tr("Page URL (optional):"), loverslab_page_url_);
    auto *hint = new QLabel(tr(
        "The numeric file id from the LoversLab file URL. The page URL "
        "lets the panel open the exact page; otherwise the bare-id URL is "
        "used."),
        page);
    hint->setWordWrap(true);
    form->addRow(hint);
    return page;
  }
  QWidget *build_steam_page() {
    auto *page = new QWidget(this);
    auto *form = new QFormLayout(page);
    steam_workshop_id_ = new QLineEdit(page);
    steam_workshop_id_->setPlaceholderText(QStringLiteral("e.g. 1234567890"));
    form->addRow(tr("Workshop ID:"), steam_workshop_id_);
    auto *hint = new QLabel(tr(
        "The numeric workshop id from "
        "https://steamcommunity.com/sharedfiles/filedetails/?id=<id>."),
        page);
    hint->setWordWrap(true);
    form->addRow(hint);
    return page;
  }

  // Map a combo index to its canonical provider-type token. Mirrors the
  // re-ordering done in the constructor: index 0 = Nexus (after re-order),
  // 1 = LoversLab, 2 = Steam, 3+ = any other registered provider in
  // SourceRegistry iteration order.
  QString provider_index_to_canonical(int idx) const {
    if (idx == 0)
      return QStringLiteral("nexus");
    if (idx == 1)
      return QStringLiteral("loverslab");
    if (idx == 2)
      return QStringLiteral("steam");
    // For unknown providers we cannot reliably know their source_type()
    // without re-iterating the registry; the simplest stable mapping is to
    // store a parallel array at construction time. For now this branch is
    // only hit when an instance has 4+ providers registered - rare.
    return {};
  }

  void on_provider_changed(int idx) {
    Q_UNUSED(idx);
    if (!field_stack_)
      return;
    const QString t = chosen_source_type();
    if (t == QLatin1String("nexus"))
      field_stack_->setCurrentWidget(nexus_page_);
    else if (t == QLatin1String("loverslab"))
      field_stack_->setCurrentWidget(loverslab_page_);
    else if (t == QLatin1String("steam"))
      field_stack_->setCurrentWidget(steam_page_);
    else
      field_stack_->setCurrentWidget(unknown_page_);
    refresh_accept_enabled();
  }

  void refresh_accept_enabled() {
    if (auto *bb = this->findChild<QDialogButtonBox *>()) {
      const QString t = chosen_source_type();
      bool ok = true;
      if (t == QLatin1String("nexus")) {
        const QString v = nexus_mod_id_->text().trimmed();
        ok = !v.isEmpty() && v.toLongLong() > 0;
      } else if (t == QLatin1String("loverslab")) {
        const QString v = loverslab_fileid_->text().trimmed();
        ok = !v.isEmpty() && v.toLongLong() > 0;
      } else if (t == QLatin1String("steam")) {
        const QString v = steam_workshop_id_->text().trimmed();
        ok = !v.isEmpty() && v.toLongLong() > 0;
      }
      // Custom / unknown providers: allow OK; they get an empty source_id
      // and rely on manual meta.ini editing.
      if (bb->button(QDialogButtonBox::Ok))
        bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
    }
  }

  ModInfoData data_;
  QComboBox *provider_combo_ = nullptr;
  QStackedWidget *field_stack_ = nullptr;
  QWidget *nexus_page_ = nullptr;
  QWidget *loverslab_page_ = nullptr;
  QWidget *steam_page_ = nullptr;
  QLabel *unknown_page_ = nullptr;
  QLineEdit *nexus_mod_id_ = nullptr;
  QLineEdit *loverslab_fileid_ = nullptr;
  QLineEdit *loverslab_page_url_ = nullptr;
  QLineEdit *steam_workshop_id_ = nullptr;
};

}  // namespace

SourceTab::SourceTab(QWidget *parent) : ModInfoTab(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  sources_ = new QTabWidget(this);
  sources_->setDocumentMode(true);
  // Intercept selection of the "+" affordance tab. The user can never
  // actually focus it: clicking it opens the add-source dialog instead,
  // and selection snaps back to the previous (real) source tab.
  connect(sources_, &QTabWidget::currentChanged, this,
          [this](int index) {
            if (plus_index_ < 0 || index != plus_index_)
              return;
            // Snap selection back to the real source tab BEFORE opening
            // the dialog so the user can never visually focus the "+"
            // affordance. QSignalBlocker prevents the recursive
            // currentChanged the setCurrentIndex below would otherwise
            // re-trigger. The blocker's destructor re-enables signals.
            const int restore = plus_index_ > 0 ? plus_index_ - 1 : 0;
            QSignalBlocker block(sources_);
            sources_->setCurrentIndex(restore);
            show_add_source_dialog();
          });
  layout->addWidget(sources_, 1);
}

SourceTab::~SourceTab() = default;

void SourceTab::set_mod(const ModInfoData &data) {
  // Contract: data is the same ModInfoData passed to set_current() by
  // ModInfoDialog before calling set_mod(). The tab reads the current mod
  // through current() (which holds that same data), so the parameter is
  // intentionally unused but kept for the ModInfoTab interface.
  Q_UNUSED(data);
  Q_ASSERT(data.id == current().id);
  populate();
  // has_data drives the tab's red-dot in the mod list (Workspace-rvld):
  // only "true" when the visible panel actually carries data.
  bool has = false;
  for (int i = 0; i < sources_->count(); ++i) {
    if (i == plus_index_)
      continue;
    auto *panel = qobject_cast<SourceInfoPanel *>(sources_->widget(i));
    if (panel && panel->has_data()) {
      has = true;
      break;
    }
  }
  set_has_data(has);
}

void SourceTab::populate() {
  // Tear down the previous layout. The "+" tab's index is also reset here
  // because every call to populate() rebuilds the full tab bar.
  plus_index_ = -1;
  while (sources_->count() > 0) {
    QWidget *page = sources_->widget(0);
    sources_->removeTab(0);
    delete page;
  }

  const QString actual_source = resolve_actual_source(current());
  if (actual_source.isEmpty()) {
    // No source attributed. Show a Manual placeholder (Workspace-fqf5:
    // manual mods must never show a Nexus tab) and the "+" affordance.
    auto *hint = new QLabel(
        tr("This mod has no download source.\n\n"
           "It is treated as a manual install. Click \"+\" to attach a "
           "source (Nexus, LoversLab, Steam Workshop, ...) if you know "
           "where this mod came from."),
        sources_);
    hint->setWordWrap(true);
    hint->setAlignment(Qt::AlignCenter);
    sources_->addTab(hint, tr("Manual"));
  } else {
    QWidget *page = build_panel_for(actual_source, current(), sources_);
    if (page == nullptr) {
      // Fallback: provider registered but the typed panel failed to
      // instantiate. Treat as no source.
      auto *hint = new QLabel(tr("No editor available for this source."),
                              sources_);
      hint->setWordWrap(true);
      sources_->addTab(hint, actual_source);
    } else {
      auto display = display_for_source(actual_source);
      const QString title = display ? display->title : actual_source;
      const QString icon_key = display ? display->icon_key : actual_source;
      add_tab_with_icon(sources_, page, title, icon_key);
    }
  }

  // The "+" affordance: a tab on the right that, when activated, opens
  // show_add_source_dialog() instead of switching view. Always present.
auto *plus_page = new QWidget(sources_);
  plus_page->setMinimumSize(0, 0);
  sources_->addTab(plus_page, QStringLiteral("+"));
  plus_index_ = sources_->count() - 1;
  // Style the "+" affordance tab. The page itself is an empty QWidget -
  // we never want to display it (the currentChanged handler snaps focus
  // back and opens the dialog). The tooltip is the only thing the user
  // sees when they hover, so make it explicit.
  if (auto *bar = sources_->tabBar()) {
    bar->setTabToolTip(plus_index_, tr("Add a source to this mod"));
  }
}

void SourceTab::first_activation() { populate(); }

void SourceTab::save_state() {
  // Skip the "+" affordance tab - it has no panel worth saving.
  for (int i = 0; i < sources_->count(); ++i) {
    if (i == plus_index_)
      continue;
    auto *panel = qobject_cast<SourceInfoPanel *>(sources_->widget(i));
    if (panel)
      panel->save_state();
  }
}

void SourceTab::show_add_source_dialog() {
  if (current().id.isEmpty())
    return;
  // Snapshot the current data; we mutate source_type/source_id on it after
  // a successful add so subsequent populate() reflects the new source.
  AddSourceDialog dialog(current(), this);
  if (dialog.exec() != QDialog::Accepted)
    return;

  const QString source_type = dialog.chosen_source_type();
  const QString source_id = dialog.chosen_source_id();
  if (source_type.isEmpty())
    return;

  // Promote the in-memory ModInfoData so the panels we are about to build
  // see the new source_type. We copy current() into a local, mutate, and
  // re-set via the public set_current() so the dialog's reload_current()
  // path on next mod-switch picks up the same values.
  ModInfoData updated = current();
  updated.source_type = source_type;
  updated.source_id = source_id;
  if (source_type == QLatin1String("loverslab")) {
    updated.source_page_url = dialog.loverslab_page_url();
  }
  set_current(updated);

  // Write the meta sidecar. Re-use the ModInfoData lambdas so this works
  // whether or not the dialog was constructed with a real save_meta.
  if (current().load_meta && current().save_meta) {
    auto meta = current().load_meta();
    meta.set("GameModManager", "source_type",
             source_type.toStdString());
    meta.set("GameModManager", "source_id", source_id.toStdString());
    // Provider-specific keys. We add the minimum the panel needs to
    // identify the mod on the new source: [Nexusmods]modid,
    // [LoversLab]fileid + page_url, [SteamWorkshop]workshop_id.
    if (source_type == QLatin1String("nexus")) {
      meta.set("Nexusmods", "modid", source_id.toStdString());
      meta.set("Nexusmods", "mod_id", source_id.toStdString());
    } else if (source_type == QLatin1String("loverslab")) {
      meta.set("LoversLab", "fileid", source_id.toStdString());
      const QString url = dialog.loverslab_page_url();
      if (!url.isEmpty())
        meta.set("LoversLab", "page_url", url.toStdString());
    } else if (source_type == QLatin1String("steam")) {
      meta.set("SteamWorkshop", "workshop_id", source_id.toStdString());
    }
    current().save_meta(meta);
  }

  // Rebuild the tab so the new source's panel replaces the Manual / old
  // panel, and the has_data() recomputes for the dialog's red-dot logic.
  populate();
  // has_data() may now have changed - recompute and notify the dialog.
  bool has = false;
  for (int i = 0; i < sources_->count(); ++i) {
    if (i == plus_index_)
      continue;
    auto *panel = qobject_cast<SourceInfoPanel *>(sources_->widget(i));
    if (panel && panel->has_data()) {
      has = true;
      break;
    }
  }
  set_has_data(has);
}

} // namespace ui