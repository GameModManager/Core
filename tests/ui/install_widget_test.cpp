// Offscreen GUI test for InstallWidget - the two-pane modpack install UI.
// Covers: the 10-step model with status glyphs, pack loading (patch consent
// mods, INI tweak groups with required locks + overridden badges, choice
// radios, instructions), diagnostics badge + inline counts, patch consent
// allow/deny, INI toggle guards, choice validation through the engine,
// prior-choice reconciliation, UpdatePlan-driven step skipping, and the
// ready_to_finish gate. Hermetic: no file access, QT_QPA_PLATFORM=offscreen
// via the test property.
#include "ui/widgets/install_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QListView>
#include <QRadioButton>
#include <QTextBrowser>

#include <catch2/catch_test_macros.hpp>

namespace
{

void check(bool cond, const char* what)
{
  INFO(what);
  REQUIRE(cond);
}

// A pack exercising every interactive step: 2 patches on 2 mods, INI tweaks
// (one required, two recommended colliding on the same key so one is
// overridden), one exactly-one and one at-most-one choice group.
engine::gmmpack::Gmmpack make_pack()
{
  engine::gmmpack::Gmmpack pack;
  pack.instructions = "# Install\n\nDo the thing.";

  engine::gmmpack::PatchEntry pa;
  pa.mod_id           = "alpha";
  pa.target_path      = "meshes/a.nif";
  pa.base_file_sha256 = std::string(64, 'a');
  pa.algorithm        = "bsdiff";
  engine::gmmpack::PatchEntry pb;
  pb.mod_id           = "beta";
  pb.target_path      = "meshes/b.nif";
  pb.base_file_sha256 = std::string(64, 'b');
  pb.algorithm        = "bsdiff";
  pack.patches        = {pa, pb};

  engine::gmmpack::IniEntry ini;
  ini.target_file = "Skyrim.ini";
  engine::gmmpack::IniTweak req;
  req.id                = "shadows";
  req.name              = "Shadows";
  req.status            = "required";
  req.enabled           = true;
  req.content           = "[Display]\niShadowMapResolution=2048\n";
  req.has_source_mod_id = false;
  engine::gmmpack::IniTweak rec_a;
  rec_a.id                = "grass-a";
  rec_a.name              = "Grass A";
  rec_a.status            = "recommended";
  rec_a.enabled           = true;
  rec_a.content           = "[Grass]\nbAllowCreateGrass=1\n";
  rec_a.source_mod_id     = "alpha";
  rec_a.has_source_mod_id = true;
  engine::gmmpack::IniTweak rec_b;
  rec_b.id                = "grass-b";
  rec_b.name              = "Grass B";
  rec_b.status            = "recommended";
  rec_b.enabled           = true;
  rec_b.content           = "[Grass]\nbAllowCreateGrass=0\n";
  rec_b.source_mod_id     = "beta";
  rec_b.has_source_mod_id = true;
  ini.tweaks              = {req, rec_a, rec_b};
  pack.ini_edits          = {ini};

  engine::gmmpack::ChoiceGroup exact;
  exact.id             = "textures";
  exact.name           = "Textures";
  exact.mode           = "exactly-one";
  exact.member_mod_ids = {"alpha", "beta"};
  engine::gmmpack::ChoiceGroup optional;
  optional.id                 = "extras";
  optional.name               = "Extras";
  optional.mode               = "at-most-one";
  optional.member_mod_ids     = {"gamma", "delta"};
  pack.manifest.choice_groups = {exact, optional};
  return pack;
}

QApplication& test_app()
{
  static int argc     = 1;
  static char argv0[] = "test";
  static char* argv[] = {argv0, nullptr};
  // Force offscreen: the shared ctest helper intends
  // QT_QPA_PLATFORM=offscreen for UI tests but a quoting bug drops it
  // (ENVIRONMENT keeps only TZ=UTC), so GUI tests would otherwise run on
  // the real display and tear down flakily. Belt and braces until the
  // helper is fixed (see follow-up ticket).
  qputenv("QT_QPA_PLATFORM", "offscreen");
  static QApplication app(argc, argv);
  return app;
}

}  // namespace

TEST_CASE("install widget step list", "[ui]")
{
  test_app();
  ui::InstallWidget widget;

  auto* list = widget.findChild<QListView*>(QStringLiteral("install_step_list"));
  check(list != nullptr, "left pane has a step list");
  REQUIRE(list != nullptr);
  check(list->model()->rowCount() == 10, "step list has 10 steps");
  for (int row = 0; row < 10; ++row) {
    const auto text = list->model()->index(row, 0).data(Qt::DisplayRole).toString();
    check(text.startsWith(QStringLiteral("\u25cb")), "steps start Pending");
  }
  check(widget.step_status(ui::InstallStep::Loot) == ui::StepStatus::Pending,
        "step_status reads back Pending");

  widget.set_step_status(ui::InstallStep::Download, ui::StepStatus::Running);
  check(widget.step_status(ui::InstallStep::Download) == ui::StepStatus::Running,
        "step_status reads back Running");
  widget.set_step_status(ui::InstallStep::Download, ui::StepStatus::Done);
  const auto done_text = list->model()->index(1, 0).data(Qt::DisplayRole).toString();
  check(done_text.startsWith(QStringLiteral("\u2713")),
        "Done step shows the check glyph");
}

TEST_CASE("install widget pack content", "[ui]")
{
  test_app();
  ui::InstallWidget widget;
  widget.set_pack(make_pack());

  // Instructions stay rendered on the right pane.
  check(widget.instructions_markdown().contains("Do the thing"),
        "instructions markdown is stored");
  auto* browser = widget.findChild<QTextBrowser*>();
  check(browser != nullptr && browser->toPlainText().contains("Do the thing"),
        "instructions render in the right pane");

  // Patch consent lists both patched mods, undecided.
  check(widget.has_patches(), "pack with patches reports has_patches");
  check(!widget.patch_consent().has_value(), "patch consent starts undecided");
  check(!widget.ready_to_finish(), "undecided consent blocks finishing");

  // INI tweaks: required locked on, recommended toggleable.
  const auto ids = widget.ini_tweak_ids();
  check(ids.size() == 3, "all three tweaks are listed");
  check(widget.ini_tweak_enabled("shadows"), "required tweak starts on");
  widget.set_ini_tweak_enabled("shadows", false);
  check(widget.ini_tweak_enabled("shadows"), "required tweak cannot be disabled");
  widget.set_ini_tweak_enabled("grass-a", false);
  check(!widget.ini_tweak_enabled("grass-a"), "recommended tweak toggles off");
  widget.set_ini_tweak_enabled("grass-a", true);

  // The required checkbox is locked in the UI too.
  bool found_locked = false;
  for (auto* box : widget.findChildren<QCheckBox*>()) {
    if (box->text().contains("Shadows")) {
      found_locked = !box->isEnabled() && box->isChecked();
    }
  }
  check(found_locked, "required tweak checkbox is locked and checked");

  // Choice radios: exactly-one empty fails, picking one validates.
  auto validation = widget.choice_validation();
  check(!widget.ready_to_finish(), "empty exactly-one group blocks finishing");
  engine::Collection::ChoicePicks picks;
  picks["textures"] = {"alpha"};
  widget.set_choice_picks(picks);
  check(widget.choice_picks().at("textures").front() == "alpha",
        "choice picks round-trip");
  validation          = widget.choice_validation();
  bool textures_valid = false;
  for (const auto& verdict : validation.verdicts) {
    if (verdict.group_id == "textures") {
      textures_valid = verdict.status == engine::Collection::ChoiceStatus::Valid;
    }
  }
  check(textures_valid, "picking one member validates the exactly-one group");

  // Radios reflect the picks.
  bool found_radio = false;
  for (auto* radio : widget.findChildren<QRadioButton*>()) {
    if (radio->text() == "alpha" && radio->isChecked()) {
      found_radio = true;
    }
  }
  check(found_radio, "radio for the picked member is checked");

  // Patch consent Allow unblocks (choices valid, no errors running).
  widget.set_patch_consent(true);
  check(widget.patch_consent().value(), "consent Allow is recorded");
  check(widget.ready_to_finish(), "valid choices + consent allow finishing");
}

TEST_CASE("install widget patch deny adds a diagnostic", "[ui]")
{
  test_app();
  ui::InstallWidget widget;
  widget.set_pack(make_pack());

  widget.set_patch_consent(false);
  check(!widget.patch_consent().value(), "consent Do Not Allow is recorded");
  check(widget.diagnostic_count() == 1, "denying patches adds one diagnostic note");
  check(widget.diagnostics().front().message.contains("unpatched"),
        "deny note says mods install unpatched");
}

TEST_CASE("install widget diagnostics badge", "[ui]")
{
  test_app();
  ui::InstallWidget widget;

  widget.add_diagnostic(ui::InstallStep::Download, "stale hash");
  widget.add_diagnostic(ui::InstallStep::Download, "missing Proton build");
  widget.add_diagnostic(ui::InstallStep::Loot, "no load order");
  check(widget.diagnostic_count() == 3, "three diagnostics recorded");

  auto* list = widget.findChild<QListView*>(QStringLiteral("install_step_list"));
  REQUIRE(list != nullptr);
  const auto text = list->model()->index(1, 0).data(Qt::DisplayRole).toString();
  check(text.contains("(2)"), "step shows its inline diagnostic count");
  check(text.contains("stale hash"), "step previews its first diagnostic inline");

  widget.clear_diagnostics();
  check(widget.diagnostic_count() == 0, "clear_diagnostics empties the list");
  const auto cleared = list->model()->index(1, 0).data(Qt::DisplayRole).toString();
  check(!cleared.contains("(2)"), "cleared step drops its count");
}

TEST_CASE("install widget incremental update skips idle steps", "[ui]")
{
  test_app();
  ui::InstallWidget widget;

  // Empty plan: nothing changed, mod steps + consent + ini + tree skip.
  engine::modpack::UpdatePlan idle;
  widget.set_update_plan(idle);
  check(widget.step_status(ui::InstallStep::Resolve) == ui::StepStatus::Skipped,
        "idle update skips resolving");
  check(widget.step_status(ui::InstallStep::Download) == ui::StepStatus::Skipped,
        "idle update skips downloading");
  check(widget.step_status(ui::InstallStep::Install) == ui::StepStatus::Skipped,
        "idle update skips installing");
  check(widget.step_status(ui::InstallStep::PatchConsent) == ui::StepStatus::Skipped,
        "idle update skips patch consent");
  check(widget.step_status(ui::InstallStep::IniEdits) == ui::StepStatus::Skipped,
        "idle update skips INI edits");
  check(widget.step_status(ui::InstallStep::BuildTree) == ui::StepStatus::Skipped,
        "idle update skips tree build");
  check(widget.step_status(ui::InstallStep::Loot) == ui::StepStatus::Pending,
        "idle update still runs LOOT");

  // Targeted plan: only the INI step runs among the skippable ones.
  engine::modpack::UpdatePlan ini_only;
  engine::modpack::IniChange change;
  change.action        = engine::modpack::IniChange::Action::Apply;
  change.target_file   = "Skyrim.ini";
  change.tweak_id      = "shadows";
  ini_only.ini_changes = {change};
  engine::gmmpack::Diagnostic diag;
  diag.severity        = engine::gmmpack::Diagnostic::Severity::Warning;
  diag.path            = "ini/Skyrim.json";
  diag.message         = "drifted value flagged";
  ini_only.diagnostics = {diag};
  widget.set_update_plan(ini_only);
  check(widget.step_status(ui::InstallStep::Resolve) == ui::StepStatus::Skipped,
        "ini-only update skips resolving");
  check(widget.step_status(ui::InstallStep::IniEdits) == ui::StepStatus::Pending,
        "ini-only update runs the INI step");
  check(widget.diagnostic_count() == 1, "plan diagnostics attach to the widget");
  check(widget.diagnostics().front().step == ui::InstallStep::IniEdits,
        "ini-path diagnostic attaches to the INI step");
}

TEST_CASE("install widget reconciles prior choices", "[ui]")
{
  test_app();
  ui::InstallWidget widget;
  widget.set_pack(make_pack());

  // Prior pick for the textures group carries forward (same membership).
  engine::Collection::PriorChoiceState prior;
  prior.picks["textures"]      = {"beta"};
  prior.membership["textures"] = {"alpha", "beta"};
  prior.membership["extras"]   = {"gamma", "delta"};
  widget.set_prior_choices(prior);
  check(widget.choice_picks().at("textures").front() == "beta",
        "unchanged group carries the prior pick forward");

  // An explicit current pick always wins over the prior.
  engine::Collection::ChoicePicks current;
  current["textures"] = {"alpha"};
  widget.set_choice_picks(current);
  widget.set_prior_choices(prior);
  check(widget.choice_picks().at("textures").front() == "alpha",
        "explicit current pick wins over the prior");
}

TEST_CASE("install widget error blocks finishing", "[ui]")
{
  test_app();
  ui::InstallWidget widget;
  widget.set_pack(make_pack());
  engine::Collection::ChoicePicks picks;
  picks["textures"] = {"alpha"};
  widget.set_choice_picks(picks);
  widget.set_patch_consent(true);
  check(widget.ready_to_finish(), "green install is ready");

  widget.set_step_status(ui::InstallStep::Download, ui::StepStatus::Error);
  check(!widget.ready_to_finish(), "an Error step blocks finishing");
  widget.set_step_status(ui::InstallStep::Download, ui::StepStatus::Done);
  check(widget.ready_to_finish(), "recovering the step unblocks finishing");
}
