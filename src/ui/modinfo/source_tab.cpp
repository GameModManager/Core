#include "ui/modinfo/source_tab.h"

#include "engine/source/source_provider.h"
#include "ui/modinfo/source_panels/generic_source_panel.h"
#include "ui/modinfo/source_panels/nexus_source_panel.h"
#include "ui/modinfo/source_panels/source_info_panel.h"
#include "ui/modinfo/source_panels/steam_source_panel.h"
#include "ui/theme/icon_manager.h"

#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cctype>
#include <string>

namespace ui {

namespace {

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

} // namespace

SourceTab::SourceTab(QWidget *parent) : ModInfoTab(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  sources_ = new QTabWidget(this);
  sources_->setDocumentMode(true);
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
  bool has = false;
  for (int i = 0; i < sources_->count(); ++i) {
    auto *panel = qobject_cast<SourceInfoPanel *>(sources_->widget(i));
    if (panel && panel->has_data()) {
      has = true;
      break;
    }
  }
  set_has_data(has);
}

void SourceTab::populate() {
  while (sources_->count() > 0) {
    QWidget *page = sources_->widget(0);
    sources_->removeTab(0);
    delete page;
  }

  // Build the visible source list from the union of the game hook's
  // supported_sources AND the sections actually present in this mod's
  // meta. Reason (Workspace-rvld): a game's download_sources knowledge
  // hook only names the providers the game's plugin wired up
  // (SkyrimSpecialEdition declares only "Nexus"; Isaac declares
  // "Nexus,Steam"). When a LoversLab mod is installed under Skyrim,
  // its [LoversLab] section is silently ignored and the user sees
  // only the Nexus tab - provenance is hidden. Without the union the
  // tab would have to chase the Plugins repo to teach every plugin
  // about every provider; with the union, provenance is visible the
  // moment the meta carries it.
  QStringList sources = current().supported_sources;
  if (current().load_meta) {
    auto meta = current().load_meta();
    auto add_unique = [&sources](const QString &s) {
      if (s.isEmpty()) return;
      if (!sources.contains(s)) sources.append(s);
    };
    if (meta.has_section("Nexusmods")) {
      add_unique(QStringLiteral("Nexus Mods"));
    }
    if (meta.has_section("LoversLab")) {
      add_unique(QStringLiteral("LoversLab"));
    }
    if (meta.has_section("SteamWorkshop")) {
      add_unique(QStringLiteral("Steam Workshop"));
    }
    const QString actual_type =
        QString::fromStdString(meta.source_type()).toLower();
    if (actual_type == QLatin1String("nexus")) {
      add_unique(QStringLiteral("Nexus Mods"));
    } else if (actual_type == QLatin1String("loverslab")) {
      add_unique(QStringLiteral("LoversLab"));
    } else if (actual_type == QLatin1String("steam") ||
               actual_type == QLatin1String("steamworkshop")) {
      add_unique(QStringLiteral("Steam Workshop"));
    }
  }
  if (sources.isEmpty()) {
    auto *hint = new QLabel(
        tr("No download sources are available for this game."), sources_);
    hint->setWordWrap(true);
    sources_->addTab(hint, tr("Sources"));
    return;
  }

  auto add_source_tab = [this](QWidget *page, const QString &title,
                               const QString &source_key) {
    const std::string vendor_key =
        engine::vendor_icon_key(source_key.toStdString());
    if (vendor_key.empty()) {
      sources_->addTab(page, title);
    } else {
      sources_->addTab(page,
                       engine::IconManager::instance().resolve_icon(
                           QString::fromStdString(vendor_key)),
                       title);
    }
  };

  for (const QString &name : sources) {
    auto *provider = find_provider(name);
    if (provider == nullptr) {
      auto *hint =
          new QLabel(tr("This source has no configurable settings."), sources_);
      hint->setWordWrap(true);
      add_source_tab(hint, name, name);
      continue;
    }
    const std::string st = provider->source_type();
    QWidget *page = nullptr;
    if (st == "nexus") {
      page = new NexusSourcePanel(current(), sources_);
    } else if (st == "steam" || st == "steamworkshop") {
      page = new SteamSourcePanel(current(), sources_);
    } else {
      page = new GenericSourcePanel(current(), provider, sources_);
    }
    add_source_tab(page, name, QString::fromStdString(provider->source_type()));
  }
}

void SourceTab::first_activation() { populate(); }

void SourceTab::save_state() {
  for (int i = 0; i < sources_->count(); ++i) {
    auto *panel = qobject_cast<SourceInfoPanel *>(sources_->widget(i));
    if (panel)
      panel->save_state();
  }
}

} // namespace ui
