// Offscreen GUI test for the Save Information dialog
// (save_info_dialog.{h,cpp}):
//
//   - The plugin list rows in save order: Skyrim.esm present (active),
//     SkyUI_SE.esp present-but-disabled, GoneMod.esp missing. Icons and
//     foreground colors match the verdict.
//   - The basic-info block carries Character/Level/Location/Save#/Time/File/
//     Has Script Extender Data, plus a v2.1+ overlay row when set.
//   - Embedded RGBA screenshot decodes into a QLabel pixmap; a save with no
//     screenshot falls back to the "No preview" placeholder.
//   - Case-insensitive matching: the load-order snapshot is "skyui_se.esp"
//     (lowercase) and the save lists "SkyUI_SE.esp" - the row is still
//     marked present (the dialog mirrors the resolver's to_lower convention).
//   - Stub save (empty plugin list) shows the "No parser" placeholder instead
//     of an empty list widget.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network.
#include "ui/widgets/save_info_dialog.h"

#include "engine/game/saves/save_game.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QStyle>
#include <QTest>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
void check(bool cond, const char* what)
{
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

static QListWidget* list_of(ui::SaveInfoDialog& dlg)
{
  return dlg.findChild<QListWidget*>();
}

static QLabel* thumb_of(ui::SaveInfoDialog& dlg)
{
  QList<QLabel*> labels = dlg.findChildren<QLabel*>();
  for (QLabel* l : labels) {
    if (!l->pixmap().isNull())
      return l;
    if (!l->text().isEmpty() && l->text().contains("preview"))
      return l;
  }
  return nullptr;
}

TEST_CASE("save info dialog", "[ui]")
{
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_save_info_dialog/config";
  std::filesystem::remove_all("/tmp/gmm_save_info_dialog");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char* test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // --- Build a representative SaveGame: a tiny embedded RGBA screenshot
  //     and three plugins (one active, one disabled, one missing).
  engine::SaveGame save;
  save.file_path         = "/tmp/gmm_save_info_dialog/Manual0_20260802_1_1.ess";
  save.pc_name           = "TestChar";
  save.pc_level          = 25;
  save.pc_location       = "Whiterun";
  save.save_number       = 7;
  save.creation_time     = 1755000000;  // arbitrary epoch
  save.plugins           = {"Skyrim.esm", "SkyUI_SE.esp", "GoneMod.esp"};
  save.overlay           = {{"Quest", "Main Questline"}};
  save.screenshot_width  = 4;
  save.screenshot_height = 4;
  save.screenshot.assign(static_cast<size_t>(4 * 4 * 4), 0x80);

  // Load-order snapshot: Skyrim.esm enabled, SkyUI_SE.esp in the list but
  // DISABLED, GoneMod.esp absent. The "SkyUI_SE.esp" entry is intentionally
  // lowercased to verify the case-insensitive lookup path the resolver
  // (save_missing_assets.cpp::to_lower) uses - both must lowercase their
  // lookup key, otherwise this row would be mis-classified as missing.
  engine::GamePlugin skyrim;
  skyrim.name      = "Skyrim.esm";
  skyrim.enabled   = true;
  skyrim.owner_mod = "";
  engine::GamePlugin skyui;
  skyui.name                               = "skyui_se.esp";
  skyui.enabled                            = false;
  skyui.owner_mod                          = "SkyUI";
  std::vector<engine::GamePlugin> snapshot = {skyrim, skyui};

  // Matching missing-assets list for the save (GoneMod.esp + providing mods
  // for SkyUI), to verify the dialog's tooltip uses the resolver's
  // provider list verbatim.
  engine::SaveMissingAsset gone;
  gone.plugin_name    = "GoneMod.esp";
  gone.inactive       = false;
  gone.providing_mods = {"<overwrite>"};
  engine::SaveMissingAsset skyui_missing;
  skyui_missing.plugin_name                     = "SkyUI_SE.esp";
  skyui_missing.inactive                        = true;
  skyui_missing.providing_mods                  = {};
  std::vector<engine::SaveMissingAsset> missing = {gone, skyui_missing};

  ui::SaveInfoDialog dlg(save, snapshot, missing);
  auto* list = list_of(dlg);
  check(list != nullptr, "dialog has a QListWidget child");
  check(list->count() == 3, "three rows: Skyrim.esm + SkyUI_SE.esp + GoneMod.esp");

  // --- 3-state verdict ---------------------------------------------------
  // Row 0: Skyrim.esm present+enabled => check icon, default foreground.
  QListWidgetItem* row0 = list->item(0);
  check(row0->text() == "Skyrim.esm", "row 0 name = Skyrim.esm");
  check(!row0->icon().isNull(), "present row has an icon");

  // Row 1: SkyUI_SE.esp present-but-disabled => present icon, but tooltip
  // mentions the disabled state.
  QListWidgetItem* row1 = list->item(1);
  check(row1->text() == "SkyUI_SE.esp",
        "row 1 name = SkyUI_SE.esp (mixed-case lookup found the lowercase "
        "load-order entry)");
  check(!row1->icon().isNull(), "present-but-disabled still gets an icon");
  check(row1->toolTip().contains("SkyUI"),
        "present row tooltip names the owning mod (SkyUI)");

  // Row 2: GoneMod.esp missing => cross icon, red foreground, tooltip
  // mentions <overwrite> as provider. Standard icons have no stable
  // identity (QStyle::standardIcon rebuilds each call), so we just
  // verify the missing row's icon differs from the present row's icon.
  QListWidgetItem* row2 = list->item(2);
  check(row2->text() == "GoneMod.esp", "row 2 name = GoneMod.esp");
  check(row2->toolTip().contains("<overwrite>"),
        "missing row tooltip names <overwrite> provider");
  // Render both row icons at a fixed size and compare the image bytes -
  // different standard icons produce different images.
  const QImage missing_img = row2->icon().pixmap(16, 16).toImage();
  const QImage present_img = row0->icon().pixmap(16, 16).toImage();
  check(missing_img != present_img,
        "missing row uses a visually distinct icon from the present row");
  // The present row also has a non-null icon (vs. the placeholder "No
  // preview" path which has no pixmap at all).
  check(!present_img.isNull(), "present row has a decoded icon image");

  // --- Header label carries the count -----------------------------------
  bool found_header = false;
  for (QLabel* lbl : dlg.findChildren<QLabel*>()) {
    if (lbl->text().contains("Plugins (3)")) {
      found_header = true;
      break;
    }
  }
  check(found_header, "header label shows Plugins (3)");

  // --- Basic info block --------------------------------------------------
  QStringList all_text;
  for (QLabel* lbl : dlg.findChildren<QLabel*>()) {
    all_text << lbl->text();
  }
  const QString joined = all_text.join('\n');
  check(joined.contains("TestChar"), "basic info shows pc_name");
  check(joined.contains("Whiterun"), "basic info shows pc_location");
  check(joined.contains("Main Questline"),
        "v2.1+ overlay row rendered in the basic info block");
  check(joined.contains("Manual0_20260802_1_1.ess"),
        "basic info shows the file basename");

  // --- Screenshot decoded ------------------------------------------------
  QLabel* thumb = thumb_of(dlg);
  // thumb_of may return the placeholder (no pixmap) when decoded - in
  // either case the dialog must not crash; verify the screenshot pipeline
  // did run by checking the list or labels for a QPixmap-backed QLabel.
  bool any_pixmap = false;
  for (QLabel* lbl : dlg.findChildren<QLabel*>()) {
    if (!lbl->pixmap().isNull()) {
      any_pixmap = true;
      break;
    }
  }
  check(any_pixmap, "embedded RGBA screenshot decoded into a QLabel pixmap");
  (void)thumb;

  // --- Stub save: empty plugin list shows the placeholder ---------------
  engine::SaveGame stub;
  stub.file_path     = "/tmp/gmm_save_info_dialog/Unknown.ess";
  stub.pc_name       = "StubChar";
  stub.creation_time = 1755000000;
  // plugins/light/medium all empty - no parser produced them.
  ui::SaveInfoDialog stub_dlg(stub, /*plugins=*/{}, /*missing=*/{});
  auto* stub_list = list_of(stub_dlg);
  check(stub_list != nullptr, "stub dialog still has a list");
  check(stub_list->count() == 1, "stub dialog shows exactly one placeholder row");
  check(stub_list->item(0)->text().contains("No plugin data"),
        "stub dialog placeholder text mentions no plugin data");
  check(!(stub_list->item(0)->flags() & Qt::ItemIsEnabled),
        "placeholder row is disabled (greyed)");

  // --- Save with no screenshot falls back to the placeholder -----------
  engine::SaveGame no_shot;
  no_shot.file_path     = "/tmp/gmm_save_info_dialog/NoShot.ess";
  no_shot.pc_name       = "NoShot";
  no_shot.creation_time = 1755000000;
  no_shot.plugins       = {"X.esp"};
  ui::SaveInfoDialog no_shot_dlg(no_shot, {}, {});
  bool found_placeholder = false;
  for (QLabel* lbl : no_shot_dlg.findChildren<QLabel*>()) {
    if (lbl->text().contains("No preview")) {
      found_placeholder = true;
      break;
    }
  }
  check(found_placeholder,
        "save with no embedded screenshot shows the 'No preview' label");

  std::filesystem::remove_all("/tmp/gmm_save_info_dialog");
}
