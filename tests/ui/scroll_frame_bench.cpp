// Scroll-frame cost measurement for the mod list (Workspace-lb45).
//
// Scrolling repaints two things every frame: the viewport (the ~30 visible
// rows) and ModMarkingScrollBar, whose paintEvent walks EVERY model row to
// draw its separator/highlight marks. This measures the model-side cost of
// one such frame so a regression has a number attached instead of a hunch.
//
// Offscreen, throwaway XDG_CONFIG_HOME, no network. Prints numbers; the
// assertions below only guard the shapes that must hold for the numbers to
// mean anything.
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QElapsedTimer>
#include <QStandardItemModel>
#include <QTreeView>

#include <cstdio>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {

// Rows a ~1080p viewport shows in the mod list.
constexpr int kVisibleRows = 30;

// The REAL library shape, lifted verbatim from a user's 222-row instance
// (Skyrim Special Edition): 125 separators followed by 97 mods, all 125
// separators consecutive at the head. '!' marks a separator. That head-heavy
// layout is the whole point - no synthetic harness built it, which is why the
// previous four attempts all measured a flat ~7 ms and missed this.
static const char kRealRows[] = R"MM(
!-- FSMP 3.5_separator|!-- FSMP 4.0_separator|
!-_______________________________________ SKYRIM DLCs & CC ________________________________________separator|
!00. _______________________________________ ROOT FOLDER __________________________________________separator|
!01. ________________________________________ CORE MODS ___________________________________________separator|
!01.1 EARLY LOADERS - CORE_separator|!01.2 MODDER RESOURCES - UTILITIES - CORE FIXES_separator|
!01.3 TOOLS_separator|!01.4 Frameworks_separator|
!02. ______________________________________ USER INTERFACE _________________________________________separator|
!02.1 CONSOLE_separator|!02.2 HUD_separator|!02.3 UI_separator|!02.4 CAMERA_separator|!02.5 MAPS_separator|
!02.6 CONTROLS_separator|
!03. _________________________________________ SOUND _____________________________________________separator|
!03.1 AUDIO - SOUND_separator|!03.2 MUSIC_separator|
!04. ___________________________________ MESHES & TEXTURES _______________________________________separator|
!04.1 BASE MESHES & TEXTURES - EARLY LOADERS_separator|!04.2 FOOD - INGREDIENTS_separator|
!04.3 POTIONS - CONSUMABLES_separator|!04.4 FURNITURE_separator|!04.5 CONTAINERS_separator|
!04.6 CLUTTER - STATIC_separator|!04.7 MISC ITEMS_separator|!04.8 BASE LOD_separator|
!05. ___________________________________ ENVIRONMENT & VFX _____________________________________separator|
!05.01 LANDSCAPE_separator|!05.02 MOUNTAINS - ROCKS_separator|!05.03 MINES - CAVES - DUNGEONS_separator|
!05.04.1 TREES_separator|!05.04.2 GRASS_separator|!05.04.3 FLORA_separator|!05.05.1 WATER_separator|
!05.05.2 SNOW & ICE_separator|!05.06 WEATHERS_separator|!05.07 SKY_separator|
!05.08 PARTICLE LIGHTS - ENB LIGHT_separator|!05.09 FIRE_separator|!05.10 OTHER VISUAL EFFECTS_separator|
!06. ________________________________ CITIES - TOWNS - VILLAGES __________________________________separator|
!06.1 GENERAL CITIES - TOWNS - VILLAGES OVERHAUL_separator|!06.2.1 CITY - SOLITUDE_separator|
!06.2.2 CITY - WHITERUN_separator|!06.2.3 CITY - MARKARTH_separator|!06.2.4 CITY - RIFTEN_separator|
!06.2.5 CITY - WINDHELM_separator|!06.3.1 MAJOR TOWN - FALKREATH_separator|
!06.3.2 MAJOR TOWN - MORTHAL_separator|!06.3.3 MAJOR TOWN - DAWNSTAR_separator|
!06.3.4 MAJOR TOWN - WINTERHOLD_separator|!06.4.1 VILLAGE - RIVERWOOD_separator|
!06.4.2 VILLAGE - DRAGON BRIDGE_separator|!06.4.3 VILLAGE - RORIKSTEAD_separator|
!06.4.4 VILLAGE - IVARSTEAD_separator|!06.4.5 VILLAGE - SHOR'S STONE_separator|
!06.4.6 VILLAGE - KYNESGROVE_separator|!06.4.7 VILLAGE - KARTHWASTEN_separator|
!06.4.8 OTHER VANILLA SETTLEMENTS_separator|!06.4.9 DLC TOWNS - VILLAGES - STRONGHOLDS_separator|
!06.5 NEW CITIES - TOWNS - VILLAGES_separator|!06.6 INTERIORS_separator|
!07. _______________________________ WORLDSPACE & LOCATIONS _________________________________separator|
!07.1 GENERAL ARCHITECTURE_separator|!07.2 VANILLA LOCATIONS OVERHAUL_separator|
!07.3 NEW LOCATIONS - NEW DUNGEONS_separator|!07.4 WORLDSPACE ADDITIONS - IMMERSION_separator|
!07.5 SHRINES - STATUES_separator|!07.6 PLAYER HOMES_separator|
!08. _____________________________________ LATE GRAPHICS _________________________________________separator|
!08.1 LIGHTING_separator|!08.2 LATE TEXTURES & MESHES_separator|
!09. ______________________________________ GAMEPLAY __________________________________________separator|
!09.1 PERKS - RACIAL - CLASSES - SKILLS_separator|!09.2 MAGIC - SHOUTS_separator|
!09.3 CRAFTING - ENCHANTING - ALCHEMY_separator|!09.4 GAMEPLAY - BALANCE_separator|
!09.5 GAMEPLAY - NEW MECHANICS_separator|!09.6 GAMEPLAY - IMMERSION_separator|
!10. ________________________________________ QUESTS ____________________________________________separator|
!10.1 DLC-SIZED MOD - NEW LANDS_separator|!10.2 QUESTS - VANILLA_separator|!10.3 QUESTS - NEW_separator|
!10.4 CIVIL WAR_separator|
!11. ______________________________________ PLAYER & NPC _________________________________________separator|
!11.1 RACEMENU_separator|!11.2 SKELETON - PHYSICS_separator|!11.3.1 BODY - HEAD - FACIAL FEATURES_separator|
!11.3.2 SKIN - TATTOO - MAKEUP_separator|!11.3.3 HAIR - BEARDS - EXTRA_separator|
!11.3.4 BODY PRESETS_separator|!11.3.5 CHARACTER PRESETS_separator|!11.4 NPC - LOOKS_separator|
!11.5 NPC - AI_separator|!11.6 NPCs - INTERACTIONS_separator|!11.7 FOLLOWERS_separator|
!12. _______________________________________ CREATURES __________________________________________separator|
!12.1 VAMPIRES - WEREWOLVES_separator|!12.2 DRAGONS_separator|!12.3 CREATURES - VANILLA_separator|
!12.4 CREATURES - NEW_separator|
!13. _________________________________ WEAPONS & ARMOURS _____________________________________separator|
!13.1 WEAPONS - ARMOUR REPLACERS_separator|!13.2 WEAPONS - NEW_separator|!13.3 ARMOURS - NEW_separator|
!13.4 CLOTHING - ITEMS_separator|!13.5 JEWELRY_separator|
!14. ________________________________ COMBAT & ANIMATIONS ____________________________________separator|
!14.1 COMBAT_separator|!14.2 COMBAT MOVESETS_separator|!14.3 MOVEMENT ANIMATIONS_separator|
!14.4 IDLES - OTHER ANIMATIONS_separator|
!15. ________________________________ GENERIC & LATE LOADERS ___________________________________separator|
!15.1 SPID DISTRIBUTION_separator|!15.2 MISCELLANEOUS PATCHES - BUG FIXES - SCRIPT FIXES_separator|
!15.3 PERFORMANCE_separator|!15.4 LATE LOADERS_separator|!15.5 MCM HELPER - SETTINGS LOADERS_separator|
!15.6 DynDOLOD_separator|
!16. ________________________________________ OTHER ______________________________________________separator|
!16.1 COMMUNITY SHADERS_separator|!16.2 SEASONS_separator|
!17. _____________________________________ TOOL OUTPUTS __________________________________________separator|
17.1 SSEEdit Output|17.2 Synthesis Output|17.3 Bodyslide Output|17.4 Pandora Output|17.5 Wrye Bash Output|
17.6 Mator Smash Output|17.9 Mod Settings|18.0 Shader Cache|A-Pose Fix|A. Tween Menu Overhaul - Classic|
Address Library All in One (1.7.104.0) v13|Animation Queue Fix|Auto Input Switch|Better Jumping (1.6.X)|
BnP female skin (Replacer+Player version)|BoS - Bimbo Lips|BodySlide and Outfit Studio - v5.8.2|
Bodyslide and Outfit Studio - Linux Native|CBBE 3BA|CBPC - Physics with Collisions|
Caliente's Beautiful Bodies Enhancer -CBBE-|Classic Sprinting Redone (Anniversary Edition)|
Community Shaders 1.8.2|ConsoleUtilSSE NG|CrashLogger|CrashLogger SSE PDB|CrosshairRefEventsFix_v0.0.1|
DAF CBBE 3BA BodySlide Presets Pack|Dawnguard.esm|
Default Female at Character Creation esl (doesn't count against your load order)|Delicate Zhuque - 3BA|
Dragonborn.esm|EngineFixes|Exit Sneak On Sprint|Faster HDT-SMP|Faster HDT-SMP 3.5|HearthFires.esm|
High Poly Head SE|Improved Camera - First Person Animations|Improved Camera SE|JContainers SE|
Jump Behavior Overhaul SE|MCM Helper|MfgFix NG|Modex - A Mod Explorer Menu - v3.0.0|
Movement Behavior Overhaul SE|OBody NG|Oathvein UI|Open Animation Replacer|Paired Animation Improvements|
Pandora Animation Cache|Pandora Behaviour Engine v4.3.1-beta|PapyrusUtil AE SE - Scripting Utility Functions|
Penetration Physics|QASmoke2 - Special Edition|RaceMenu  - Body Options|RaceMenu Anniversary Edition|
SMP-Fixes-0.0.3 - for - FSMP 3.0.0-Beta|SSE Display Tweaks|Screen Space GI|
SexLab Framework PPLUS - V2.17.1 - NG|SexLabFrameworkAE_v166b|SexLabP+ Animations|Skeleton Auto Patch|
Skooma Whore SE v1.01|SkyHUD|SkyParkour V3 - Parkour Framework|SkyUI|Skylighting|Skyrim Defaults|
Skyrim Script Extender (SKSE64) Steam|Skyrim.esm|Smart Talk - Latest Version|
Soaking Wet for 1.6.629 and newer|Spell Perk Item Distributor|Sprint Sneak Movement Speed Fix|
Stamina On Standby|True Directional Movement|TrueHUD|UIExtensions v1-2-0|
Unofficial Skyrim Special Edition Patch|Update.esm|Water Collision Crash Fix|Wetness Effects|
Widescreen Scale Removed|XP32 Maximum Skeleton Special Extended|[Predator] SMP Head|_ResourcePack.esl|
ccBGSSSE001-Fish.esm|ccBGSSSE025-AdvDSGS.esm|ccBGSSSE037-Curios.esl|ccQDRSSE001-SurvivalMode.esl|
dTry's Key Utils AE-NG|powerofthree's Papyrus Extender|powerofthree's Tweaks|skse64_2_02_06|trainwreck
)MM";

// '!' marks a separator; everything else is a mod row.
//
// `version_links` adds the multi-version folders: the 97 mods become runs of
// three where the 2nd and 3rd are versions of the 1st, linked by parent_id.
// The real modlist.txt carries no parent_id (MO2 models versions as siblings
// under a separator, not as children), so the quadratic parent-chain walks are
// unreachable on this list as shipped - that is the finding, and `links=false`
// is what proves it. `links=true` is the shape where those walks DO run, so
// the guard covers the case that would regress if it ever were reachable.
void populate(ui::ModList &model, bool nested, bool fold, bool version_links,
              bool updates = false, bool max_badges = false) {
  model.set_nesting_enabled(nested);
  QVector<ui::ModEntry> entries;
  int n = 0;
  for (const QString &token :
       QString::fromUtf8(kRealRows).split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
    // The literal is line-wrapped, so a token can carry a leading newline.
    const QString raw = token.trimmed();
    ui::ModEntry m;
    const bool sep    = raw.startsWith(QLatin1Char('!'));
    m.id              = sep ? QStringLiteral("sep_%1").arg(n) : raw;
    m.name            = sep ? raw.mid(1) : raw;
    m.is_separator    = sep;
    m.separator_color = "#3a5f8a";
    // Only a separator with a hideable row under it can fold; the head run of
    // 125 back-to-back separators has an empty band on all but the last.
    m.folded           = fold && sep && m.id != QStringLiteral("sep_0");
    m.conflict_wins    = (n % 7 == 0) ? 1 : 0;
    m.conflict_losses  = (n % 11 == 0) ? 1 : 0;
    m.has_hidden_files = (n % 13 == 0);
    m.is_fomod         = (n % 17 == 0);
    // Every mod row wears the update badge. This is the state PR #302/#305
    // added and the one the benchmark NEVER set: without it shows_update() is
    // false everywhere, the whole version-cell block (DecorationRole icon +
    // a freshly built bold QFont + a QBrush) is never reached, and the
    // benchmark reports a green frame for a list no user has after running an
    // update check. A real library carries these badges on a large share of its
    // rows for weeks afterwards.
    if (updates && !sep) {
      m.version                = QStringLiteral("1.%1").arg(n % 20);
      m.newest_version         = QStringLiteral("2.%1").arg(n % 20);
      m.nexus_update_available = true;
    }
    // Worst-case badge load: every flag the Flags column can draw a badge for,
    // all at once. The set of flags grew over time (hidden files, FOMOD, root
    // override, mirror, mirror-source-gone, toggle error, invalid, not-installed
    // by manager), so the per-row badge count is the one mod-list-only,
    // mod-count-independent quantity that plausibly moved. data() pushes up to
    // eight QIcons here and FlagsDelegate paints each one per frame.
    if (max_badges && !sep) {
      m.has_hidden_files      = true;
      m.is_empty              = true;
      m.is_fomod              = true;
      m.root_override         = true;
      m.is_mirrored           = true;
      m.mirror_source_missing = true;
      m.toggle_error          = QStringLiteral("write failed");
      m.invalid_data          = true;
      m.conflict_wins         = 1;
      m.conflict_losses       = 1;
    }
    // Multi-version folder: versions 2 and 3 of each run hang off version 1.
    // Only the mod tail is versioned, and the head separators stay untouched
    // so the 125-empty-separator run is preserved exactly.
    if (version_links && !sep && (n - 125) >= 2 && ((n - 125) % 3) != 0)
      m.parent_id =
          QStringLiteral("%1").arg(entries.at(entries.size() - ((n - 125) % 3)).id);
    entries.append(m);
    ++n;
  }
  model.reset_with_order(entries);
}

// One ModMarkingScrollBar::paintEvent pass: isRowHidden + kScrollMarkRole for
// every row in the model (mod_table_view.cpp:274-290).
double scrollbar_frame_ms(const ui::ModList &model) {
  QElapsedTimer t;
  t.start();
  for (int r = 0; r < model.rowCount(); ++r) {
    const QVariant v = model.index(r, 0).data(ui::ModList::kScrollMarkRole);
    if (v.canConvert<QColor>())
      (void)v.value<QColor>();
  }
  return t.nsecsElapsed() / 1e6;
}

// One viewport pass: the roles QStyledItemDelegate reads for each visible cell.
double viewport_frame_ms(const ui::ModList &model, int first_row) {
  static const int roles[] = {Qt::DisplayRole,
                              Qt::DecorationRole,
                              Qt::FontRole,
                              Qt::BackgroundRole,
                              Qt::CheckStateRole,
                              Qt::ToolTipRole,
                              Qt::TextAlignmentRole,
                              Qt::ForegroundRole,
                              ui::ModList::kFlagIconsRole,
                              ui::ModList::kIndentDepthRole,
                              ui::ModList::kIsLastChildRole,
                              ui::ModList::kIsSeparatorRole,
                              ui::ModList::kSourceIconsRole};
  QElapsedTimer t;
  t.start();
  for (int r = first_row; r < first_row + kVisibleRows && r < model.rowCount(); ++r) {
    for (int c = 0; c < ui::ModList::ColumnCount; ++c) {
      const QModelIndex idx = model.index(r, c);
      for (int role : roles)
        (void)idx.data(role);
    }
  }
  return t.nsecsElapsed() / 1e6;
}

}  // namespace

// End-to-end: one SmoothScroller tick is scrollbar->setValue() followed by a
// synchronous repaint of the viewport AND the scrollbar. That is the real
// per-frame cost, delegates and layout included.
//
// `with_marking_bar` false swaps in a plain QScrollBar, which is the A/B that
// isolates ModMarkingScrollBar's O(all rows) mark pass from the rest.
double scroll_frame_ms(QAbstractItemView &view, int steps, int step_px,
                       bool with_marking_bar) {
  auto *bar = view.verticalScrollBar();
  QElapsedTimer t;
  t.start();
  for (int i = 0; i < steps; ++i) {
    const int next = bar->value() + step_px;
    if (next > bar->maximum())
      bar->setValue(bar->minimum());
    else
      bar->setValue(next);
    // Synchronous paints: exactly what the event loop does per frame.
    if (with_marking_bar)
      bar->repaint();
    else
      view.verticalScrollBar()->repaint();
    view.viewport()->repaint();
  }
  return t.nsecsElapsed() / 1e6 / steps;
}

TEST_CASE("mod list scroll frame cost", "[perf]") {
  if (!qEnvironmentVariableIsSet("GMM_BENCH_NATIVE_DISPLAY"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("TZ", "UTC");
  tzset();
  const std::filesystem::path cfg = "/tmp/gmm_scroll_bench/config";
  std::filesystem::remove_all("/tmp/gmm_scroll_bench");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int argc     = 1;
  char arg0[]  = "test";
  char *argv[] = {arg0, nullptr};
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // --- Frame cost on a realistic list (the user's shape) ---------------------
  // A frame budget is 16.7 ms at 60 Hz. The previous harness measured 7 ms
  // and flat on a synthetic flat list with one separator per 25 rows and
  // NOTHING folded; the real library is dense in separators and folded.
  constexpr int kSteps = 60;
  printf("[scroll-e2e] realistic list: separator-dense + multi-version folders\n");
  for (bool nested : {false, true}) {
    for (bool fold : {false, true}) {
      for (bool links : {false, true}) {
        // The user's 222-row list: 125 separators, 97 mods.
        ui::ModList model;
        populate(model, nested, fold, links);
        const int rows = model.rowCount();
        int seps       = 0;
        for (int i = 0; i < rows; ++i)
          seps += model.mods().at(i).is_separator ? 1 : 0;
        REQUIRE(rows == 222);
        REQUIRE(seps == 125);

        ui::ModView modview;
        modview.resize(1600, 1000);
        modview.setModel(&model);
        modview.show();
        // apply_fold_state() needs the view bound (it drives setRowHidden), so
        // folding is only real once the model is attached. Without this the
        // folded flags are inert and the harness measures the wrong list.
        model.apply_fold_state();
        QApplication::processEvents();

        // A/B: the SAME model in a bare QTreeView with no custom scrollbar and
        // no custom delegates. Whatever gap exists here is ours to fix; whatever
        // both pay is Qt's per-cell style painting and is not.
        QTreeView plain;
        plain.resize(1600, 1000);
        plain.setModel(&model);
        plain.show();
        QApplication::processEvents();

        // Control: the SAME row and column count in an identically-sized bare
        // QTreeView, over a model that answers DisplayRole and nothing else.
        // QStandardItemModel holds no font, brush, alignment, decoration or
        // checkbox role, so this is what Qt costs to paint this many cells with
        // the stock delegate and none of our roles in the path. Whatever
        // `plain_ms` costs ABOVE this is the price of what ModList::data
        // answers, and that difference is the only per-cell cost in this file
        // that belongs to us.
        QStandardItemModel flat(rows, ui::ModList::ColumnCount);
        for (int r = 0; r < rows; ++r)
          for (int c = 0; c < ui::ModList::ColumnCount; ++c)
            flat.setData(
                flat.index(r, c),
                QStringLiteral("cell %1").arg(r * ui::ModList::ColumnCount + c),
                Qt::DisplayRole);
        QTreeView flatview;
        flatview.resize(1600, 1000);
        flatview.setModel(&flat);
        flatview.show();
        QApplication::processEvents();

        (void)scroll_frame_ms(modview, 10, 20, true);
        (void)scroll_frame_ms(plain, 10, 20, false);
        (void)scroll_frame_ms(flatview, 10, 20, false);

        const double ms       = scroll_frame_ms(modview, kSteps, 20, true);
        const double plain_ms = scroll_frame_ms(plain, kSteps, 20, false);
        const double flat_ms  = scroll_frame_ms(flatview, kSteps, 20, false);

        // The model-side cost, measured without any rendering. This is the part
        // we own, and it is the only part a regression in this file can move.
        // The end-to-end frame above is ~98% Qt per-cell style painting (drawRow
        // -> drawControl -> subElementRect -> viewItemSize), so it is far too
        // noisy to guard: a reintroduced O(N^2) scan moved it by ~0.7 ms, well
        // inside run-to-run noise, and the frame-time assertion stayed green.
        const double model_ms = viewport_frame_ms(model, 0);
        const double bar_ms   = scrollbar_frame_ms(model);

        // Scaling law, not an absolute: the same 30 visible rows' worth of work,
        // done across the WHOLE list (222 rows = 7.4 batches) instead of one
        // batch. A model whose per-cell cost is independent of list size scales
        // by the batch ratio (~7); a model that rescans the list per cell scales
        // by the square (~55). This is the assertion that can actually fail:
        // an absolute budget cannot, because even a FULLY quadratic model costs
        // only ~0.7 ms on a 222-row list - the list is too small for O(N^2) to
        // hurt, which is exactly why the quadratic hypothesis is not the cause.
        double wide_ms = 0.0;
        for (int k = 0; k * kVisibleRows < rows; ++k)
          wide_ms += viewport_frame_ms(model, k * kVisibleRows);
        const double batches = static_cast<double>(rows) / kVisibleRows;
        const double ratio   = wide_ms / model_ms;

        printf("[scroll-e2e] rows=%3d nesting=%d folded=%d links=%d  ModView=%7.3f ms  "
               "plain=%7.3f ms  flat=%7.3f ms  roles=%6.3f ms  ours=%6.3f ms  "
               "model=%6.3f ms  markbar=%6.3f ms  scale=%.1fx (linear=%.1fx)\n",
               rows, nested ? 1 : 0, fold ? 1 : 0, links ? 1 : 0, ms, plain_ms, flat_ms,
               plain_ms - flat_ms, ms - plain_ms, model_ms, bar_ms, ratio, batches);

        INFO("realistic frame at " << rows << " rows, nesting=" << nested
                                   << " folded=" << fold << " links=" << links
                                   << " was " << ms << " ms");
        // Frame: a gross-regression tripwire only (measured 9.0-11.9 ms here,
        // against a 16.7 ms budget), because rendering noise dwarfs model cost.
        REQUIRE(ms < 16.7);
        // Model: per-cell cost must not depend on list size. A quadratic
        // regression lands near batches^2; keep well clear of that.
        REQUIRE(ratio < batches * 3.0);
        REQUIRE(bar_ms < 5.0);
      }
    }
  }

  // --- Two list shapes the loop above never builds ---------------------------
  // Both are directly comparable to the nesting=0 folded=0 links=0 line above,
  // which is the identical list with neither. The delta is what the badge work
  // costs per frame once the roles that carry it are live.
  //
  // UPDATE BADGE is the state PR #302/#305 added and the benchmark never set:
  // without it shows_update() is false everywhere, so the version cell's
  // DecorationRole + freshly built bold QFont + QBrush are never reached and the
  // benchmark reports a green frame for a list no user has after an update check.
  //
  // MAX BADGES gives every row all eight Flags badges, which is what the badge
  // set grew into and what the ~21% one-badge rows above understate.
  const auto shaped = [&](const char *label, bool updates, bool max_badges) {
    ui::ModList model;
    populate(model, false, false, false, updates, max_badges);
    REQUIRE(model.rowCount() == 222);

    ui::ModView modview;
    modview.resize(1600, 1000);
    modview.setModel(&model);
    modview.show();
    QApplication::processEvents();

    QTreeView plain;
    plain.resize(1600, 1000);
    plain.setModel(&model);
    plain.show();
    QApplication::processEvents();

    (void)scroll_frame_ms(modview, 10, 20, true);
    (void)scroll_frame_ms(plain, 10, 20, false);

    const double ms       = scroll_frame_ms(modview, kSteps, 20, true);
    const double plain_ms = scroll_frame_ms(plain, kSteps, 20, false);
    printf("[scroll-e2e] %-12s ModView=%7.3f ms  plain=%7.3f ms  ours=%6.3f ms\n",
           label, ms, plain_ms, ms - plain_ms);
  };
  shaped("UPDATE BADGE", true, false);
  shaped("MAX BADGES", false, true);
}