#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>
#include <QStringList>

class QTabWidget;

namespace ui {

// The source the mod is attributed to: the one whose section carries
// primary=true, else the first recorded one. A mod with a single source is
// implicitly primary, so this is never empty for a mod that has one. Free
// function (not a method) because it is the answer to "what sources does this
// mod have", which the tab bar, the add-source dialog and a test all ask.
[[nodiscard]] QString primary_source(const ModInfoData &data);

// Record `source_type` as the mod's primary source: its own provider section
// carries primary=true and every other source's section has the flag removed,
// so setting one demotes the previous and two primaries cannot exist. Returns
// false when the source has no section to flag (it was never attached).
bool apply_primary_source(const ModInfoData &data, const QString &source_type);

// Detach `source_type` from the mod: its provider section is dropped, and so is
// the mod's declared attribution when it named that source. Nothing in the mod
// folder is read or written, so the mod stays installed with every file it has.
// Returns false when the source has no section to drop.
bool detach_source(const ModInfoData &data, const QString &source_type);

// MO2's Nexus tab generalized into a per-mod Source tab. Shows one tab per
// source the mod actually has (its download source, a git working copy, or
// both - and more than one provider when the mod carries several), plus a "+"
// tab on the right that lets the user add another source manually. The previous
// "show every game-supported source" behavior fabricated Nexus presence on
// every mod (Workspace-fqf5): a manual / Steam / LoversLab mod under a
// Nexus-enabled game used to display a Nexus tab even though no Nexus
// provenance existed, and users conflated tab visibility with actual
// source attribution.
class SourceTab : public ModInfoTab {
  Q_OBJECT
public:
  explicit SourceTab(QWidget *parent = nullptr);
  ~SourceTab() override;

  void set_mod(const ModInfoData &data) override;
  void first_activation() override;
  void save_state() override;

private:
  // Build (or rebuild) the source tabs plus the "+" affordance tab
  // from the current ModInfoData and the mod's meta. Called whenever the
  // displayed mod changes or after the user attaches a new source via "+".
  void populate();

  // Open a modal dialog that lets the user attach a Nexus / LoversLab /
  // Steam source to the current mod. On confirm, writes the appropriate
  // provider section + [GameModManager]source_type/source_id via
  // current().save_meta(), updates the in-memory ModInfoData, then
  // repopulates the tab.
  void show_add_source_dialog();

  // Right-click menu on a source tab: make that source the mod's primary one,
  // or detach the source from the mod. Both act on the source_type the tab
  // index maps to in tab_sources_.
  void show_source_menu(const QPoint &pos);
  void set_primary_source(const QString &source_type);
  void delete_source(const QString &source_type);

  QTabWidget *sources_ = nullptr;
  // Index of the "+" tab inside sources_, -1 when none. Stored so the
  // currentChanged handler can detect when the user tried to activate it
  // and intercept (open the dialog) without leaving the tab focused.
  int plus_index_ = -1;
  // source_type of each content tab, in tab order, so the context menu can
  // name the source the user actually right-clicked. Empty for the Manual
  // placeholder (not a source); the "+" affordance has no entry at all.
  QStringList tab_sources_;
};

}  // namespace ui
