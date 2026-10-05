#include "ui/modinfo/source_tab.h"

#include "engine/mod/meta/mod_meta.h"
#include "engine/source/git/git_info.h"
#include "engine/source/source_provider.h"
#include "ui/modinfo/mod_info_data.h"
#include "ui/modinfo/source_panels/generic_source_panel.h"
#include "ui/modinfo/source_panels/git_source_panel.h"
#include "ui/modinfo/source_panels/loverslab_source_panel.h"
#include "ui/modinfo/source_panels/modpub_source_panel.h"
#include "ui/modinfo/source_panels/nexus_source_panel.h"
#include "ui/modinfo/source_panels/source_info_panel.h"
#include "ui/modinfo/source_panels/steam_source_panel.h"
#include "ui/theme/icon_manager.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <filesystem>
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

  // Add a tab to sources_ wearing `icon_key`, the FINAL icon key - the name of
  // a file under resources/icons/vendor/ ("nexusmods", "steam", "github",
  // "git"), never a source_type. Mapping a source_type to that key is the
  // CALL SITE's job: feeding an already-final key back through
  // vendor_icon_key() drops it, because that function only knows the five
  // vendor source types. An empty key adds the tab with no icon, which is
  // what resolve_icon("") returns.
  void add_tab_with_icon(QTabWidget *tabs, QWidget *page, const QString &title,
                         const QString &icon_key) {
    tabs->addTab(page, engine::IconManager::instance().resolve_icon(icon_key), title);
  }

  // Whether this mod has a Git source. A .git inside the mod folder is the
  // authoritative signal for being a working copy - that is what the panel's
  // check / pull / reset act on, and what the mod list's git badge follows. A
  // recorded [Git] section counts too, because it is a source the user
  // attached: the panel then states that the folder holds no repository
  // instead of pretending the mod has no Git source at all. data_.is_git is
  // the controller's own scan of the folder, kept as the last resort.
  bool mod_is_git(const ModInfoData &data) {
    // A default-constructed QDir reports "." as its path, and the process's
    // working directory is not a mod folder - so a caller that never set
    // mod_dir must not be read as "the repo we happen to be standing in".
    const QString dir_path = data.mod_dir.path();
    if (!dir_path.isEmpty() && dir_path != QLatin1String(".")) {
      const std::filesystem::path dir(dir_path.toStdString());
      if (engine::Git::is_repository(dir))
        return true;
    }
    if (data.load_meta && data.load_meta().has_git())
      return true;
    return data.is_git;
  }

  // The upstream URL the git badge is picked from: the live repository's
  // remote when there is one, else the recorded [Git] remote_url. Empty for a
  // repo with no configured remote, which shows the generic git badge.
  QString git_remote_url(const ModInfoData &data) {
    const QString dir_path = data.mod_dir.path();
    if (!dir_path.isEmpty() && dir_path != QLatin1String(".")) {
      const std::filesystem::path dir(dir_path.toStdString());
      const std::string live = engine::Git::remote_url(dir);
      if (!live.empty())
        return QString::fromStdString(live);
    }
    if (!data.git_remote_url.isEmpty())
      return data.git_remote_url;
    if (data.load_meta)
      return QString::fromStdString(data.load_meta().git_remote_url());
    return {};
  }

  // The meta section that holds one source's own record: the same
  // per-provider sidecar sections every source already writes. Empty for
  // anything that is not a real source ("manual", "direct").
  QString provider_section(const QString &source_type) {
    if (source_type == QLatin1String("nexus"))
      return QStringLiteral("Nexusmods");
    if (source_type == QLatin1String("loverslab"))
      return QStringLiteral("LoversLab");
    if (source_type == QLatin1String("steam"))
      return QStringLiteral("SteamWorkshop");
    if (source_type == QLatin1String("modpub"))
      return QStringLiteral("ModPub");
    if (source_type == QLatin1String("git"))
      return QStringLiteral("Git");
    return {};
  }

  // Every source this mod actually has, primary first.
  //
  // A mod can carry several at once - a Nexus id and a Steam workshop id, plus
  // a .git in its folder - and each keeps its own provider section, so this
  // reads all of them instead of stopping at the first. A section counts when
  // it holds a real id: modid=0 is MO2's "no Nexus id" sentinel, not
  // provenance, and an empty id is nothing. The declared
  // [GameModManager]source_type counts on its own (a mod can be attributed to
  // a source whose id is not stamped yet), and data_.source_type is the last
  // resort for a mod whose meta has not been stamped at all.
  //
  // The primary leads the list, so the tab the user picked comes first. A mod
  // with one source is implicitly primary - nothing to pick, nothing recorded.
  // A meta that somehow flags two sections resolves to the first of them, so
  // the tab bar can never show two primaries.
  QStringList recorded_sources(const ModInfoData &data) {
    static const QStringList kOrder = {
        QStringLiteral("nexus"), QStringLiteral("loverslab"), QStringLiteral("steam"),
        QStringLiteral("modpub"), QStringLiteral("git")};
    QStringList found;
    // Only the four providers that own a typed panel are sources here.
    // "manual" and "direct" are not a source (direct is the transport-only
    // modl:// provider), and an unknown source_type falls through to the
    // generic panel rather than becoming a tab of its own here.
    auto is_source = [](const QString &type) {
      return type == QLatin1String("nexus") || type == QLatin1String("loverslab") ||
             type == QLatin1String("steam") || type == QLatin1String("modpub");
    };
    auto add = [&found](const QString &type) {
      if (!type.isEmpty() && !found.contains(type))
        found.append(type);
    };
    engine::ModMeta meta;
    if (data.load_meta) {
      meta       = data.load_meta();
      auto id_at = [&meta](const char *section, const char *key) {
        return QString::fromStdString(meta.get(section, key)).toLongLong();
      };
      if (id_at("Nexusmods", "modid") > 0)
        add(QStringLiteral("nexus"));
      if (id_at("LoversLab", "fileid") > 0)
        add(QStringLiteral("loverslab"));
      if (id_at("SteamWorkshop", "workshop_id") > 0)
        add(QStringLiteral("steam"));
      if (id_at("ModPub", "mod_id") > 0)
        add(QStringLiteral("modpub"));
      const QString declared = QString::fromStdString(meta.source_type()).toLower();
      if (is_source(declared))
        add(declared);
    }
    const QString in_memory = data.source_type.toLower();
    if (is_source(in_memory))
      add(in_memory);
    if (mod_is_git(data))
      add(QStringLiteral("git"));

    // Everything the fixed order does not know (a plugin's own provider) keeps
    // the order it was found in, behind the known ones.
    QStringList ordered;
    for (const auto &type : kOrder) {
      if (found.contains(type))
        ordered.append(type);
    }
    for (const auto &type : std::as_const(found)) {
      if (!ordered.contains(type))
        ordered.append(type);
    }

    if (ordered.size() < 2)
      return ordered;
    // The section that names itself primary wins over the order above, which
    // is the display order only.
    QString primary;
    if (data.load_meta) {
      for (const auto &type : std::as_const(ordered)) {
        const QString section = provider_section(type);
        if (section.isEmpty())
          continue;
        if (meta.get(section.toStdString(), "primary") == "true") {
          primary = type;
          break;
        }
      }
    }
    if (primary.isEmpty())
      primary = ordered.first();
    ordered.removeOne(primary);
    ordered.prepend(primary);
    return ordered;
  }

  // What detaching a source actually costs, in the stored fields' own terms:
  // every key below is written back only by re-adding the source and fetching
  // its metadata again. Unknown providers fall back to naming their section,
  // because guessing at their fields would be a lie.
  QString loss_summary(const QString &type) {
    if (type == QLatin1String("nexus"))
      return QStringLiteral("the recorded Nexus mod id, the Nexus metadata fetched "
                            "for it (description, category, version, page dates) and "
                            "the recorded installed-file list");
    if (type == QLatin1String("steam"))
      return QStringLiteral("the recorded Steam Workshop id and the metadata "
                            "stored for it (title, description, preview image, tags, "
                            "page dates)");
    if (type == QLatin1String("loverslab"))
      return QStringLiteral("the recorded LoversLab file id and page URL, and the "
                            "metadata stored for it (display name, author, category, "
                            "description, archive filename, page date)");
    if (type == QLatin1String("modpub"))
      return QStringLiteral("the recorded mod.pub id, page URL and game slug, and the "
                            "metadata stored for it (display name, author, category, "
                            "description, page date)");
    if (type == QLatin1String("git"))
      return QStringLiteral("the recorded remote URL, branch and commit");
    const QString section = provider_section(type);
    return section.isEmpty()
               ? QStringLiteral("the mod's recorded attribution to %1").arg(type)
               : QStringLiteral("everything stored in [%1]").arg(section);
  }

  // Build a panel for the given source_type, using the typed SourceInfoPanel
  // subclass when one exists and a GenericSourcePanel otherwise. The single
  // tab the user sees - the rest of the Source tab is the "+" affordance.
  QWidget *build_panel_for(const QString &source_type, const ModInfoData &data,
                           QWidget *parent) {
    if (source_type == QLatin1String("nexus")) {
      return new NexusSourcePanel(data, parent);
    }
    if (source_type == QLatin1String("loverslab")) {
      return new LoversLabSourcePanel(data, parent);
    }
    if (source_type == QLatin1String("steam")) {
      return new SteamSourcePanel(data, parent);
    }
    if (source_type == QLatin1String("modpub")) {
      return new ModPubSourcePanel(data, parent);
    }
    if (source_type == QLatin1String("git")) {
      return new GitSourcePanel(data, parent);
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
    // "direct" is a transport-only provider (modl:// flow) - not a
    // user-attributable source. Treat it like Manual.
    if (source_type == QLatin1String("direct"))
      return std::nullopt;
    for (auto *provider : engine::SourceRegistry::instance().providers()) {
      const QString pt = QString::fromStdString(provider->source_type()).toLower();
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
  //
  // Provider mapping strategy (Workspace-fqf5 review fix):
  //   The combo stores each item's canonical source_type ("nexus" /
  //   "loverslab" / "steam" / ...) in Qt::UserRole via addItem(display,
  //   canonical). chosen_source_type() simply reads currentData(). This
  //   avoids hardcoded positional indices and survives arbitrary registry
  //   orderings or missing providers (e.g. a Nexus-only build with no
  //   LoversLab registered). Priority order in the combo (Nexus first,
  //   then LoversLab, then Steam, then everything else in registration
  //   order) is enforced by sorting an Entry{display,canonical} vector
  //   before populating the combo - no in-place re-ordering that could
  //   mis-track other items' indices.
  class AddSourceDialog : public QDialog {
  public:
    AddSourceDialog(const ModInfoData &data, QWidget *parent)
        : QDialog(parent), data_(data) {
      setWindowTitle(tr("Add Source"));
      auto *layout = new QVBoxLayout(this);

      auto *intro = new QLabel(
          tr("Attach this mod to a download source. The selected provider's "
             "metadata will be written to the mod's sidecar and the Source tab "
             "will reload with the new source."),
          this);
      intro->setWordWrap(true);
      layout->addWidget(intro);

      auto *form      = new QFormLayout();
      provider_combo_ = new QComboBox(this);

      // Every source the mod ALREADY has, per the same helper the tab bar
      // uses to decide which panels to build. Asking it instead of
      // re-deriving "what sources does this mod have" keeps one notion of it -
      // which is also what makes the de-duplication below cover a mod that
      // carries several sources rather than just the one it declares.
      const QStringList existing = recorded_sources(data);

      // Build the sorted Entry list from the registry. We normalize
      // "steamworkshop" -> "steam" so the canonical key the rest of the
      // codebase expects (and that SourceInfoPanel guards on) is consistent
      // regardless of how a Steam plugin reports itself.
      struct Entry {
        QString display;
        QString canonical;
        int priority = 0;
      };
      auto priority_for = [](const QString &canonical) {
        if (canonical == QLatin1String("nexus"))
          return 0;
        if (canonical == QLatin1String("loverslab"))
          return 1;
        if (canonical == QLatin1String("steam"))
          return 2;
        if (canonical == QLatin1String("modpub"))
          return 3;
        if (canonical == QLatin1String("git"))
          return 4;
        return 5;
      };
      QList<Entry> entries;
      for (auto *provider : engine::SourceRegistry::instance().providers()) {
        Entry e;
        e.display  = QString::fromStdString(provider->display_name());
        QString pt = QString::fromStdString(provider->source_type()).toLower();
        if (pt == QLatin1String("steamworkshop"))
          pt = QStringLiteral("steam");
        // "direct" is the transport-only provider used by the modl:// flow;
        // it is not a user-attributable source (a "Direct" tag carries no
        // useful identity). Skip it in the Add Source combo.
        if (pt == QLatin1String("direct"))
          continue;
        // Never offer a source the mod already has. The existing source is
        // untouched - its panel stays on the tab bar, fully visible and
        // editable - only the add affordance goes away, so once the source is
        // detached the entry comes straight back into this combo.
        if (existing.contains(pt))
          continue;
        e.canonical = pt;
        e.priority  = priority_for(pt);
        entries.append(e);
      }
      // Git is not a download provider - nothing is fetched from a repository -
      // so no SourceProvider is registered for it and the loop above cannot
      // produce the entry. It is a source all the same: it owns the [Git]
      // section and its own tab. Offered directly, and skipped for a mod that
      // already has one (a .git in the folder, or a recorded [Git] section),
      // which is what existing.contains("git") already reports.
      if (!existing.contains(QStringLiteral("git"))) {
        entries.append(
            Entry{QStringLiteral("Git"), QStringLiteral("git"), priority_for("git")});
      }
      std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.priority != b.priority)
          return a.priority < b.priority;
        return a.display.compare(b.display, Qt::CaseInsensitive) < 0;
      });
      for (const auto &e : entries) {
        provider_combo_->addItem(e.display, e.canonical);
      }
      form->addRow(tr("Provider:"), provider_combo_);
      layout->addLayout(form);

      // The fields stack swaps based on the chosen provider. Each provider
      // contributes a small QWidget built lazily and added to the stack; we
      // rebuild on combo change so edits do not silently carry over.
      field_stack_ = new QStackedWidget(this);
      layout->addWidget(field_stack_, 1);

      nexus_page_     = build_nexus_page();
      loverslab_page_ = build_loverslab_page();
      steam_page_     = build_steam_page();
      modpub_page_    = build_modpub_page();
      git_page_       = build_git_page();
      field_stack_->addWidget(nexus_page_);
      field_stack_->addWidget(loverslab_page_);
      field_stack_->addWidget(steam_page_);
      field_stack_->addWidget(modpub_page_);
      field_stack_->addWidget(git_page_);
      // Map canonical -> field page index. Unknown providers (custom plugins)
      // get an empty page with an "edit in meta.ini" hint.
      page_by_canonical_[QStringLiteral("nexus")]     = 0;
      page_by_canonical_[QStringLiteral("loverslab")] = 1;
      page_by_canonical_[QStringLiteral("steam")]     = 2;
      page_by_canonical_[QStringLiteral("modpub")]    = 3;
      page_by_canonical_[QStringLiteral("git")]       = 4;
      unknown_page_                                   = new QLabel(
          tr("This provider has no editable fields here. After confirming, the "
             "mod's source_type will be set and you can finish configuration by "
             "editing the meta sidecar directly."),
          this);
      unknown_page_->setWordWrap(true);
      field_stack_->addWidget(unknown_page_);
      page_by_canonical_[QString()] = field_stack_->count() - 1;

      connect(provider_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
              &AddSourceDialog::on_provider_changed);
      on_provider_changed(provider_combo_->currentIndex());

      auto *buttons =
          new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
      layout->addWidget(buttons);
      connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

      // OK is disabled until at least the minimum required field is filled
      // (the current provider's id field). Refresh on every edit and on
      // provider change.
      connect(nexus_mod_id_, &QLineEdit::textChanged, this,
              &AddSourceDialog::refresh_accept_enabled);
      connect(loverslab_fileid_, &QLineEdit::textChanged, this,
              &AddSourceDialog::refresh_accept_enabled);
      connect(steam_workshop_id_, &QLineEdit::textChanged, this,
              &AddSourceDialog::refresh_accept_enabled);
      connect(modpub_mod_id_, &QLineEdit::textChanged, this,
              &AddSourceDialog::refresh_accept_enabled);
      connect(modpub_page_url_, &QLineEdit::textChanged, this,
              &AddSourceDialog::refresh_accept_enabled);
      connect(git_remote_, &QLineEdit::textChanged, this,
              &AddSourceDialog::refresh_accept_enabled);
      refresh_accept_enabled();
    }

    // The provider that the user picked, in the canonical short form used
    // for [GameModManager]source_type ("nexus" / "loverslab" / "steam" / ...).
    // Reads the canonical token stored in Qt::UserRole itemData, so the
    // answer is stable regardless of the combo's visible order.
    QString chosen_source_type() const {
      if (!provider_combo_)
        return {};
      return provider_combo_->currentData().toString();
    }

    // Identifier for the chosen provider. For Nexus this is the mod id; for
    // LoversLab it is the file id; for Steam it is the workshop id; for
    // ModPub it is the numeric mod id. Custom providers always get an
    // empty id and rely on the user editing meta.ini.
    QString chosen_source_id() const {
      const QString t = chosen_source_type();
      if (t == QLatin1String("nexus"))
        return nexus_mod_id_->text().trimmed();
      if (t == QLatin1String("loverslab"))
        return loverslab_fileid_->text().trimmed();
      if (t == QLatin1String("steam"))
        return steam_workshop_id_->text().trimmed();
      if (t == QLatin1String("modpub"))
        return modpub_mod_id_->text().trimmed();
      return {};
    }

    // Per-provider secondary fields. May be empty when the user did not
    // enter them (LoversLab page_url, ModPub page_url, Steam none).
    QString loverslab_page_url() const { return loverslab_page_url_->text().trimmed(); }
    QString modpub_page_url() const { return modpub_page_url_->text().trimmed(); }
    // The repository a newly attached Git source came from. Git has no numeric
    // id, so chosen_source_id() stays empty for it.
    QString git_remote() const { return git_remote_->text().trimmed(); }

  private:
    QWidget *build_nexus_page() {
      auto *page    = new QWidget(this);
      auto *form    = new QFormLayout(page);
      nexus_mod_id_ = new QLineEdit(page);
      nexus_mod_id_->setPlaceholderText(QStringLiteral("e.g. 12345"));
      form->addRow(tr("Mod ID:"), nexus_mod_id_);
      auto *hint = new QLabel(tr("The numeric mod id from the mod's Nexus URL. "
                                 "https://www.nexusmods.com/<game>/mods/<id>."),
                              page);
      hint->setWordWrap(true);
      form->addRow(hint);
      return page;
    }
    QWidget *build_loverslab_page() {
      auto *page        = new QWidget(this);
      auto *form        = new QFormLayout(page);
      loverslab_fileid_ = new QLineEdit(page);
      loverslab_fileid_->setPlaceholderText(QStringLiteral("e.g. 12345"));
      form->addRow(tr("File ID:"), loverslab_fileid_);
      loverslab_page_url_ = new QLineEdit(page);
      loverslab_page_url_->setPlaceholderText(
          QStringLiteral("https://www.loverslab.com/files/file/12345/"));
      form->addRow(tr("Page URL (optional):"), loverslab_page_url_);
      auto *hint = new QLabel(
          tr("The numeric file id from the LoversLab file URL. The page URL "
             "lets the panel open the exact page; otherwise the bare-id URL is "
             "used. When provided, must start with http:// or https://."),
          page);
      hint->setWordWrap(true);
      form->addRow(hint);
      return page;
    }
    QWidget *build_steam_page() {
      auto *page         = new QWidget(this);
      auto *form         = new QFormLayout(page);
      steam_workshop_id_ = new QLineEdit(page);
      steam_workshop_id_->setPlaceholderText(QStringLiteral("e.g. 1234567890"));
      form->addRow(tr("Workshop ID:"), steam_workshop_id_);
      auto *hint =
          new QLabel(tr("The numeric workshop id from "
                        "https://steamcommunity.com/sharedfiles/filedetails/?id=<id>."),
                     page);
      hint->setWordWrap(true);
      form->addRow(hint);
      return page;
    }
    QWidget *build_modpub_page() {
      auto *page     = new QWidget(this);
      auto *form     = new QFormLayout(page);
      modpub_mod_id_ = new QLineEdit(page);
      modpub_mod_id_->setPlaceholderText(QStringLiteral("e.g. 22"));
      form->addRow(tr("Mod ID:"), modpub_mod_id_);
      modpub_page_url_ = new QLineEdit(page);
      modpub_page_url_->setPlaceholderText(
          QStringLiteral("https://mod.pub/skyrim-se/22-stay-at-the-system-page-ng"));
      form->addRow(tr("Page URL (optional):"), modpub_page_url_);
      auto *hint = new QLabel(
          tr("The numeric mod id from the mod.pub page URL. The page URL is "
             "strongly recommended: it carries the game-slug and the slug-suffix, "
             "neither of which can be reconstructed from the id alone. When "
             "provided, must start with http:// or https://."),
          page);
      hint->setWordWrap(true);
      form->addRow(hint);
      return page;
    }
    QWidget *build_git_page() {
      auto *page  = new QWidget(this);
      auto *form  = new QFormLayout(page);
      git_remote_ = new QLineEdit(page);
      git_remote_->setPlaceholderText(
          QStringLiteral("https://github.com/user/repo.git"));
      form->addRow(tr("Remote URL:"), git_remote_);
      auto *hint = new QLabel(
          tr("Where this mod's files come from. A .git in the mod folder is what "
             "makes the mod a git working copy - this only records which "
             "repository, so the tab appears even when the folder holds no "
             "repository of its own."),
          page);
      hint->setWordWrap(true);
      form->addRow(hint);
      return page;
    }

    void on_provider_changed(int idx) {
      Q_UNUSED(idx);
      if (!field_stack_)
        return;
      const QString t = chosen_source_type();
      auto it         = page_by_canonical_.find(t);
      if (it != page_by_canonical_.end()) {
        field_stack_->setCurrentIndex(it.value());
      } else {
        field_stack_->setCurrentWidget(unknown_page_);
      }
      refresh_accept_enabled();
    }

    void refresh_accept_enabled() {
      if (auto *bb = this->findChild<QDialogButtonBox *>()) {
        const QString t = chosen_source_type();
        // Nothing to add (every registered provider is already attached, so
        // the combo is empty) - OK would have nothing to write.
        bool ok = !t.isEmpty();
        if (t == QLatin1String("nexus")) {
          const QString v = nexus_mod_id_->text().trimmed();
          ok              = !v.isEmpty() && v.toLongLong() > 0;
        } else if (t == QLatin1String("loverslab")) {
          const QString v = loverslab_fileid_->text().trimmed();
          ok              = !v.isEmpty() && v.toLongLong() > 0;
          if (ok) {
            // Optional page_url: when provided, must be http(s). We only
            // reject when the user typed something but it parses to a
            // non-web scheme (file://, javascript:, data:, ...). The URL
            // is later handed to QDesktopServices::openUrl().
            const QString url = loverslab_page_url_->text().trimmed();
            if (!url.isEmpty()) {
              const QUrl parsed(url);
              const QString scheme = parsed.scheme().toLower();
              if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
                ok = false;
              }
            }
          }
        } else if (t == QLatin1String("steam")) {
          const QString v = steam_workshop_id_->text().trimmed();
          ok              = !v.isEmpty() && v.toLongLong() > 0;
        } else if (t == QLatin1String("modpub")) {
          const QString v = modpub_mod_id_->text().trimmed();
          ok              = !v.isEmpty() && v.toLongLong() > 0;
          if (ok) {
            // Optional page_url: when provided, must be http(s). Same scheme
            // gate as LoversLab. The bare-id fallback is acceptable but a
            // page URL is the recommended form (it carries the game-slug
            // and the slug-suffix).
            const QString url = modpub_page_url_->text().trimmed();
            if (!url.isEmpty()) {
              const QUrl parsed(url);
              const QString scheme = parsed.scheme().toLower();
              if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
                ok = false;
              }
            }
          }
        } else if (t == QLatin1String("git")) {
          // No id to validate: a remote may be any git transport
          // (https://, ssh://, git@host:path), so requiring http(s) here
          // would reject the scp-style form. A remote is all there is to
          // record, so an empty one means there is nothing to attach.
          ok = !git_remote_->text().trimmed().isEmpty();
        }
        // Custom / unknown providers: allow OK; they get an empty source_id
        // and rely on manual meta.ini editing.
        if (bb->button(QDialogButtonBox::Ok))
          bb->button(QDialogButtonBox::Ok)->setEnabled(ok);
      }
    }

    ModInfoData data_;
    QComboBox *provider_combo_     = nullptr;
    QStackedWidget *field_stack_   = nullptr;
    QWidget *nexus_page_           = nullptr;
    QWidget *loverslab_page_       = nullptr;
    QWidget *steam_page_           = nullptr;
    QWidget *modpub_page_          = nullptr;
    QWidget *git_page_             = nullptr;
    QLabel *unknown_page_          = nullptr;
    QLineEdit *nexus_mod_id_       = nullptr;
    QLineEdit *loverslab_fileid_   = nullptr;
    QLineEdit *loverslab_page_url_ = nullptr;
    QLineEdit *steam_workshop_id_  = nullptr;
    QLineEdit *modpub_mod_id_      = nullptr;
    QLineEdit *modpub_page_url_    = nullptr;
    QLineEdit *git_remote_         = nullptr;
    // Canonical source_type -> index in field_stack_. Always populated
    // for the well-known providers; an empty-string entry points at the
    // unknown-provider hint page.
    QMap<QString, int> page_by_canonical_;
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
  connect(sources_, &QTabWidget::currentChanged, this, [this](int index) {
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
  // Right-click a source tab to make it primary or detach it. The bar has its
  // own menu: the widget-level menu would carry the tab bar's own items.
  sources_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(sources_->tabBar(), &QWidget::customContextMenuRequested, this,
          &SourceTab::show_source_menu);
  layout->addWidget(sources_, 1);
}

QString primary_source(const ModInfoData &data) {
  return recorded_sources(data).value(0);
}

bool apply_primary_source(const ModInfoData &data, const QString &source_type) {
  if (!data.load_meta || !data.save_meta)
    return false;
  const QString section = provider_section(source_type);
  auto meta             = data.load_meta();
  if (section.isEmpty())
    return false;
  // Exactly one section carries the flag: setting a new primary clears the
  // flag from every other source, so the previous one is demoted in the same
  // write and two primaries cannot survive, not even in a hand-edited meta.
  const QStringList recorded = recorded_sources(data);
  if (!recorded.contains(source_type))
    return false;
  for (const auto &other : recorded) {
    const QString other_section = provider_section(other);
    if (other_section.isEmpty())
      continue;
    if (other == source_type)
      meta.set(other_section.toStdString(), "primary", "true");
    else
      meta.unset(other_section.toStdString(), "primary");
  }
  return data.save_meta(meta);
}

bool detach_source(const ModInfoData &data, const QString &source_type) {
  if (!data.load_meta || !data.save_meta)
    return false;
  const QString section = provider_section(source_type);
  auto meta             = data.load_meta();
  if (section.isEmpty() || !meta.has_section(section.toStdString()))
    return false;
  // Read before the section goes: [General]modid is the Nexus id namespace
  // ONLY, so a leftover entry there would keep claiming the id the mod is
  // about to give up.
  const std::string nexus_modid = meta.get("Nexusmods", "modid");
  const bool clears_general     = section == QLatin1String("Nexusmods") &&
                                  !nexus_modid.empty() && nexus_modid != "0" &&
                                  meta.get("General", "modid") == nexus_modid;
  meta.clear_section(section.toStdString());
  // The mod's declared attribution pointed at this source. Leaving it would
  // keep naming a source the mod no longer has, in the mod list's badge and
  // every reader of [GameModManager]source_type.
  if (meta.source_type() == source_type.toStdString()) {
    meta.set("GameModManager", "source_type", "manual");
    meta.unset("GameModManager", "source_id");
  }
  if (clears_general)
    meta.unset("General", "modid");
  // Nothing else: no file in the mod folder is read or written here, so
  // detaching a source cannot uninstall the mod or touch its data.
  return data.save_meta(meta);
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
  // Freeze painting while the tab bar is torn down and rebuilt (every mod
  // switch deletes the old source panel and constructs a new one, including
  // a fresh QWebEngineView). Without this the user sees the teardown frames
  // as a quick close-open flicker of the whole Source tab. Updates are
  // re-enabled at the end of this function.
  sources_->setUpdatesEnabled(false);
  plus_index_ = -1;
  while (sources_->count() > 0) {
    QWidget *page = sources_->widget(0);
    sources_->removeTab(0);
    delete page;
  }

  // Every source the mod actually has, one tab each, the primary first.
  // A mod can be a git working copy AND have download sources, or carry a
  // Nexus id and a Steam workshop id; each keeps its own section, so each gets
  // its own tab. Git is in the list too (a recorded [Git] section or a .git in
  // the folder), which is why a git-only mod shows a single "Git" tab and
  // never a "Manual" placeholder beside it.
  const QStringList sources = recorded_sources(current());
  const QString primary     = sources.value(0);

  if (sources.isEmpty()) {
    // No source attributed. Show a Manual placeholder (Workspace-fqf5: manual
    // mods must never show a Nexus tab) and the "+" affordance.
    auto *hint =
        new QLabel(tr("This mod has no download source.\n\n"
                      "It is treated as a manual install. Click \"+\" to attach a "
                      "source (Nexus, LoversLab, Steam Workshop, ...) if you know "
                      "where this mod came from."),
                   sources_);
    hint->setWordWrap(true);
    hint->setAlignment(Qt::AlignCenter);
    sources_->addTab(hint, tr("Manual"));
    // A placeholder is not a source: the context menu must skip it.
    tab_sources_.append(QString());
  }

  for (int i = 0; i < sources.size(); ++i) {
    const QString type = sources.at(i);
    QString title;
    QString icon_key;
    if (type == QLatin1String("git")) {
      // The title is always "Git" - GitHub, GitLab and a self-hosted server are
      // the same source with a different badge, so the platform never names a
      // tab. icon_key_for() reads the remote's host and returns the FINAL icon
      // key ("github" for github.com, "git" for every other host), which is
      // exactly what add_tab_with_icon() takes.
      title    = tr("Git");
      icon_key = GitSourcePanel::icon_key_for(git_remote_url(current()));
    } else {
      auto display = display_for_source(type);
      title        = display ? display->title : type;
      // display->icon_key is a source_type ("nexus", "loverslab", "steam",
      // "modpub"), so it still needs the map to a vendor icon key.
      icon_key = QString::fromStdString(
          engine::vendor_icon_key((display ? display->icon_key : type).toStdString()));
    }
    // A mod with one source is implicitly primary, so the marker only appears
    // where there is a choice to make - which is also the only case where the
    // context menu offers the action.
    const bool is_primary = (type == primary) && sources.size() > 1;

    QWidget *page = build_panel_for(type, current(), sources_);
    if (page == nullptr) {
      // Fallback: provider registered but the typed panel failed to
      // instantiate. The source still has a tab, so it stays visible and
      // removable.
      auto *hint = new QLabel(tr("No editor available for this source."), sources_);
      hint->setWordWrap(true);
      page = hint;
    }
    add_tab_with_icon(sources_, page, is_primary ? title + tr(" (primary)") : title,
                      icon_key);
    tab_sources_.append(type);
    auto *bar = sources_->tabBar();
    if (bar != nullptr && is_primary) {
      // indexOf, not the loop index: a mod with no source carries a Manual
      // placeholder ahead of the real tabs.
      bar->setTabToolTip(sources_->indexOf(page),
                         tr("Primary source: the one the mod is attributed to."));
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
  sources_->setUpdatesEnabled(true);
}

void SourceTab::first_activation() {
  populate();
}

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

void SourceTab::show_source_menu(const QPoint &pos) {
  if (current().id.isEmpty())
    return;
  auto *bar       = sources_->tabBar();
  const int index = bar->tabAt(pos);
  // The "+" affordance (past the end of tab_sources_) and the Manual
  // placeholder (an empty entry) have nothing to act on.
  if (index < 0 || index >= tab_sources_.size() || tab_sources_.at(index).isEmpty())
    return;
  const QString type        = tab_sources_.at(index);
  const QStringList sources = recorded_sources(current());
  const bool is_primary     = sources.value(0) == type;

  QMenu menu(this);
  // The action stays visible when it cannot do anything - it says why there.
  if (sources.size() < 2)
    menu.addSection(tr("This is the mod's only source, so it is primary already"));
  else if (is_primary)
    menu.addSection(tr("Already the primary source"));
  auto *set_primary = menu.addAction(tr("Set as primary source"));
  set_primary->setEnabled(sources.size() > 1 && !is_primary);
  menu.addSeparator();
  auto *del = menu.addAction(tr("Delete this source..."));

  QAction *picked = menu.exec(bar->mapToGlobal(pos));
  if (picked == set_primary)
    set_primary_source(type);
  else if (picked == del)
    delete_source(type);
}

void SourceTab::set_primary_source(const QString &type) {
  if (!apply_primary_source(current(), type))
    return;
  // The tab bar leads with the primary, so it reorders.
  populate();
}

void SourceTab::delete_source(const QString &type) {
  if (!current().load_meta || !current().save_meta)
    return;
  auto meta             = current().load_meta();
  const QString section = provider_section(type);
  if (section.isEmpty() || !meta.has_section(section.toStdString()))
    return;

  // What the confirmation states, counted BEFORE anything is written, so the
  // dialog cannot describe a write that then does something else.
  // [General]modid is the Nexus id namespace ONLY, and detaching a Nexus source
  // clears the entry there too when it holds that same id.
  const std::string nexus_modid = meta.get("Nexusmods", "modid");
  const bool clears_general = type == QLatin1String("nexus") && !nexus_modid.empty() &&
                              nexus_modid != "0" &&
                              meta.get("General", "modid") == nexus_modid;
  // What is left, for the "and afterwards" half of the dialog.
  QStringList remaining;
  const QStringList recorded = recorded_sources(current());
  for (const auto &other : recorded) {
    if (other != type)
      remaining.append(other);
  }
  // A .git in the folder outranks the recorded section, so deleting a Git
  // source off a real working copy removes the record but not the tab.
  const bool repo_survives = type == QLatin1String("git") && mod_is_git(current()) &&
                             !current().mod_dir.path().isEmpty() &&
                             current().mod_dir.path() != QLatin1String(".") &&
                             engine::Git::is_repository(std::filesystem::path(
                                 current().mod_dir.path().toStdString()));

  QString after;
  if (repo_survives) {
    after = tr("The mod folder holds a .git, and that is what makes this a Git "
               "source, so the Git tab stays. Only what was recorded about the "
               "repository goes.");
  } else if (remaining.isEmpty()) {
    after = tr("The mod is left with no source: it is treated as a manual "
               "install and its tab shows the Manual placeholder.");
  } else {
    // Whatever is left leads the tab bar instead, so name it - that is the
    // source the mod will be attributed to.
    QStringList names;
    for (const auto &other : std::as_const(remaining)) {
      auto display = display_for_source(other);
      names.append(display ? display->title : other);
    }
    after = names.size() == 1
                ? tr("The mod keeps %1, which becomes its primary source.")
                      .arg(names.first())
                : tr("The mod keeps %1; %2 becomes its primary source.")
                      .arg(names.join(", "), names.first());
  }

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("Delete this source"));
  box.setText(
      tr("Detach %1 from this mod?")
          .arg(display_for_source(type) ? display_for_source(type)->title : type));
  box.setInformativeText(
      tr("You lose %1%2. Nothing in the mod folder is deleted or changed: the mod "
         "stays installed with every file it has.\n\n%3\n\nThis cannot be undone "
         "except by attaching the source again and fetching its metadata afresh.")
          .arg(loss_summary(type),
               clears_general ? tr(", and the mod id in [General]") : QString(),
               after));
  auto *yes    = box.addButton(tr("Delete source"), QMessageBox::AcceptRole);
  auto *cancel = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
  // Focus lands on Cancel: the destructive path must be the deliberate one.
  box.setDefaultButton(cancel);
  box.exec();
  if (box.clickedButton() != yes)
    return;

  detach_source(current(), type);

  // The in-memory copy still names the deleted source, and populate() reads
  // it, so the tab would come straight back.
  ModInfoData updated = current();
  if (updated.source_type.toLower() == type)
    updated.source_type = QStringLiteral("manual");
  updated.source_id.clear();
  set_current(updated);
  populate();

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

void SourceTab::show_add_source_dialog() {
  if (current().id.isEmpty())
    return;
  // Snapshot the current data; we mutate source_type/source_id on it after
  // a successful add so subsequent populate() reflects the new source.
  AddSourceDialog dialog(current(), this);
  if (dialog.exec() != QDialog::Accepted)
    return;

  const QString source_type = dialog.chosen_source_type();
  const QString source_id   = dialog.chosen_source_id();
  if (source_type.isEmpty())
    return;

  // Promote the in-memory ModInfoData so the panels we are about to build
  // see the new source_type. We copy current() into a local, mutate, and
  // re-set via the public set_current() so the dialog's reload_current()
  // path on next mod-switch picks up the same values.
  //
  // Git is deliberately left out: it is provenance beside whatever the mod
  // was installed from, not a replacement for it, so it never claims
  // [GameModManager]source_type. A git-managed manual mod stays manual.
  ModInfoData updated = current();
  if (source_type != QLatin1String("git")) {
    updated.source_type = source_type;
    updated.source_id   = source_id;
  }
  if (source_type == QLatin1String("loverslab")) {
    updated.source_page_url = dialog.loverslab_page_url();
  } else if (source_type == QLatin1String("modpub")) {
    updated.source_page_url = dialog.modpub_page_url();
  }
  set_current(updated);

  // Write the meta sidecar. Re-use the ModInfoData lambdas so this works
  // whether or not the dialog was constructed with a real save_meta.
  if (current().load_meta && current().save_meta) {
    auto meta = current().load_meta();
    if (source_type != QLatin1String("git")) {
      meta.set("GameModManager", "source_type", source_type.toStdString());
      meta.set("GameModManager", "source_id", source_id.toStdString());
    }
    // Provider-specific keys. We add the minimum the panel needs to
    // identify the mod on the new source: [Nexusmods]modid,
    // [LoversLab]fileid + page_url, [SteamWorkshop]workshop_id,
    // [ModPub]mod_id + page_url, [Git]remote_url.
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
    } else if (source_type == QLatin1String("modpub")) {
      meta.set("ModPub", "mod_id", source_id.toStdString());
      const QString url = dialog.modpub_page_url();
      if (!url.isEmpty())
        meta.set("ModPub", "page_url", url.toStdString());
    } else if (source_type == QLatin1String("git")) {
      // Only the remote: the section itself is what makes the mod a Git
      // source, and commit/branch are stamped by the Git panel from the
      // repository itself.
      meta.set("Git", "remote_url", dialog.git_remote().toStdString());
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

}  // namespace ui