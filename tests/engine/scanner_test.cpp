// Engine regression test for separator + mod metadata scanning.
//
// Pins the MO2-parity contract:
//   - separators are detected by the folder's "<name>_separator" suffix and
//     their display name is the folder minus the suffix
//     (ModList::getDisplayName),
//   - a separator's color comes from its meta.ini [General] color key (the same
//     file MO2's ModInfoRegular::setColor writes) and stays EMPTY when no color
//     is stored - it is never defaulted to "#888888" anymore (that fallback
//     lives in the UI model rendering),
//   - regular mods are scanned from meta.ini and carry no color.
#include "engine/game/detect/mod_scanner.h"
#include "engine/game/registry/game_knowledge.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const std::string &msg) {
  INFO(msg);
  REQUIRE(cond);
}
} // namespace

static void write_file(const fs::path &p, const std::string &contents) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p);
  out << contents;
  require(out.good(), "write_file failed for " + p.string());
}

static const engine::ScannedMod *
by_folder(const std::vector<engine::ScannedMod> &mods,
          const std::string &folder) {
  for (const auto &m : mods)
    if (m.folder_name == folder)
      return &m;
  return nullptr;
}

TEST_CASE("scanner", "[engine]") {
  const fs::path root = "/tmp/gmm_scanner_test";
  fs::remove_all(root);
  fs::create_directories(root);

  // Separator with a color in its meta.ini.
  fs::create_directories(root / "My Mods_separator");
  write_file(root / "My Mods_separator" / "meta.ini",
             "[General]\ncolor = #ff888888\n");

  // Separator with no meta.ini at all - no color.
  fs::create_directories(root / "Plain_separator");

  // Regular mod with a meta.ini - has a [General] section but no color.
  fs::create_directories(root / "SomeMod");
  write_file(root / "SomeMod" / "meta.ini",
             "[General]\nversion = 1.0\nmodid = 0\n");

  engine::GameKnowledge knowledge;
  const auto mods = engine::ModScanner::scan_dir(knowledge, "testgame", root);

  const auto *colored = by_folder(mods, "My Mods_separator");
  require(colored != nullptr, "colored separator found");
  require(colored->is_separator, "My Mods_separator is a separator");
  require(colored->display_name == "My Mods",
          "display name is folder minus the separator suffix");
  require(colored->separator_color == "#ff888888",
          "color read from meta.ini [General] color");

  const auto *plain = by_folder(mods, "Plain_separator");
  require(plain != nullptr, "plain separator found");
  require(plain->is_separator, "Plain_separator is a separator");
  require(plain->display_name == "Plain", "plain separator display name");
  require(plain->separator_color.empty(),
          "no color default - separator_color stays empty");

  const auto *mod = by_folder(mods, "SomeMod");
  require(mod != nullptr, "regular mod found");
  require(!mod->is_separator, "SomeMod is not a separator");
  require(mod->display_name == "SomeMod", "mod display name is the folder");
  require(mod->separator_color.empty(), "regular mod carries no color");
  require(!mod->is_fomod, "plain meta.ini is not flagged FOMOD");

  // A FOMOD-installed mod: meta.ini carries [fomod] choices= (written by
  // install_stage). It must be flagged so the mod list can show the wizard.
  fs::create_directories(root / "FomodMod");
  write_file(root / "FomodMod" / "meta.ini",
             "[General]\nversion = 2.0\n[fomod]\nchoices = {\"step\":\"x\"}\n");
  const auto mods3 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *fm = by_folder(mods3, "FomodMod");
  require(fm != nullptr, "fomod mod found");
  require(fm->is_fomod, "mod with [fomod] choices is flagged FOMOD");
  require(fm->version == "2.0", "fomod mod keeps its version");

  // A mod whose meta.ini has a [fomod] section but no choices key is not
  // flagged - only the persisted-choice marker counts.
  fs::create_directories(root / "EmptyFomod");
  write_file(root / "EmptyFomod" / "meta.ini",
             "[General]\n[fomod]\nalwaysRestore = 1\n");
  const auto mods4 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *ef = by_folder(mods4, "EmptyFomod");
  require(ef != nullptr, "empty-fomod mod found");
  require(!ef->is_fomod, "[fomod] without choices is not flagged FOMOD");

  // A separator whose meta.ini exists but has no color key.
  fs::create_directories(root / "NoColor_separator");
  write_file(root / "NoColor_separator" / "meta.ini", "[General]\n");
  const auto mods2 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *nc = by_folder(mods2, "NoColor_separator");
  require(nc != nullptr, "no-color separator found");
  require(nc->separator_color.empty(),
          "meta.ini without a color key yields an empty color");

  // A mod disabled via the default GMM sentinel. No game plugin declared a
  // disable_mechanism here, so the engine default (.gmmdisabled) must apply:
  // the mod comes back disabled on rescan (was a silent no-op before).
  fs::create_directories(root / "DisabledMod");
  write_file(root / "DisabledMod" / "meta.ini", "[General]\n");
  write_file(root / "DisabledMod" / ".gmmdisabled", "");
  const auto mods5 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *dm = by_folder(mods5, "DisabledMod");
  require(dm != nullptr, "disabled mod found");
  require(!dm->enabled, "mod carrying .gmmdisabled is disabled by default");

  // GMM-internal scratch dirs (.gmm_overlay_work and friends) are manager
  // machinery, never mods - wherever they land in the mods dir (an overlay
  // launch can leave .gmm_overlay_work here as an artifact).
  fs::create_directories(root / ".gmm_overlay_work");
  fs::create_directories(root / ".gmm_staging");
  const auto modsG = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  require(by_folder(modsG, ".gmm_overlay_work") == nullptr,
          ".gmm_overlay_work is not listed as a mod");
  require(by_folder(modsG, ".gmm_staging") == nullptr,
          ".gmm_staging is not listed as a mod");

  // A mod flagged rootOverride in its meta.ini [General].
  fs::create_directories(root / "RootMod");
  write_file(root / "RootMod" / "meta.ini", "[General]\nrootOverride = 1\n");
  const auto mods6 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *rm = by_folder(mods6, "RootMod");
  require(rm != nullptr, "root-flagged mod found");
  require(rm->root_override, "mod with [General] rootOverride=1 is flagged");

  // A mod without the key stays unflagged.
  fs::create_directories(root / "FlatMod");
  write_file(root / "FlatMod" / "meta.ini", "[General]\n");
  const auto mods7 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *flat = by_folder(mods7, "FlatMod");
  require(flat != nullptr, "flat mod found");
  require(!flat->root_override, "mod without rootOverride stays unflagged");

  // A folder dropped into Mods/ with NO metadata file must still be listed
  // (MO2 lists every folder) - it is flagged so the UI can warn it wasn't
  // installed by the manager. Was silently invisible before.
  fs::create_directories(root / "DroppedFolder");
  const auto modsA = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *df = by_folder(modsA, "DroppedFolder");
  require(df != nullptr, "meta-less folder is still listed");
  require(df->no_metadata, "meta-less folder flagged no_metadata");
  require(df->display_name == "DroppedFolder",
          "meta-less folder display name is the folder");
  require(!df->invalid_data,
          "no checker registered -> nothing can look invalid");

  // A malformed meta.ini must not hide the folder either (MO2's QSettings
  // reads it as empty): it keeps its defaults instead of vanishing.
  fs::create_directories(root / "BrokenMeta");
  write_file(root / "BrokenMeta" / "meta.ini", "this is {{{ not valid ini ]]]");
  const auto modsB = engine::ModScanner::scan_dir(knowledge, "testgame", root);
  const auto *bm = by_folder(modsB, "BrokenMeta");
  require(bm != nullptr, "malformed meta.ini folder still listed");
  require(!bm->no_metadata,
          "malformed meta.ini still counts as metadata present");
  require(!bm->invalid_data,
          "no checker registered -> malformed folder not invalid");

  // Content-validity markers come from per-game hooks (the engine never
  // hardcodes them): mod_valid_dirs + mod_valid_exts model the Bethesda
  // allow-set. No markers -> no checker -> no folder can be invalid.
  engine::GameKnowledge checker;
  checker.set("skyrim", "mod_valid_dirs", "textures,meshes");
  checker.set("skyrim", "mod_valid_exts", "esp,esm");

  // Whitelist approach: empty folder with no metadata and no valid game data
  // is NOT listed at all (MO2 parity — only folders with recognized content
  // appear in the mod list).
  fs::create_directories(root / "BadContent");
  const auto modsC = engine::ModScanner::scan_dir(checker, "skyrim", root);
  const auto *bc = by_folder(modsC, "BadContent");
  require(bc == nullptr, "empty content folder with no metadata is NOT listed");

  // A folder with a game plugin file is valid content but still has no
  // manager metadata (the two flags are independent). Subdirectories alone
  // (meshes/, scripts/, etc.) are NOT enough — vanilla game dirs have those.
  fs::create_directories(root / "GoodContent");
  fs::create_directories(root / "GoodContent" / "textures");
  write_file(root / "GoodContent" / "modfile.esp", "");
  const auto modsC2 = engine::ModScanner::scan_dir(checker, "skyrim", root);
  const auto *gc = by_folder(modsC2, "GoodContent");
  require(gc != nullptr && !gc->invalid_data,
          "folder with .esp and textures/ is valid content");
  require(gc->no_metadata, "folder with .esp still flagged no_metadata");

  // A meta.ini-only folder is valid content (metadata presence counts,
  // mirroring MO2's Bethesda checker accepting meta.ini via "ini").
  fs::create_directories(root / "MetaOnly");
  write_file(root / "MetaOnly" / "meta.ini", "[General]\n");
  const auto modsC3 = engine::ModScanner::scan_dir(checker, "skyrim", root);
  const auto *mo = by_folder(modsC3, "MetaOnly");
  require(mo != nullptr && !mo->invalid_data,
          "meta.ini-only folder is valid content");
  require(!mo->no_metadata, "meta.ini-only folder has metadata");

  // XML metadata games (Isaac): metadata_file hook + Isaac's content markers.
  engine::GameKnowledge xml_know;
  xml_know.set("isaac", "metadata_file", "metadata.xml");
  xml_know.set("isaac", "metadata_name_tag", "name");
  xml_know.set("isaac", "metadata_version_tag", "version");
  xml_know.set("isaac", "mod_valid_dirs", "resources,resources-dlc3");

  // Whitelist approach: folder without metadata.xml and no valid content
  // is NOT listed at all.
  fs::create_directories(root / "XmlNoMeta");
  const auto modsX = engine::ModScanner::scan_dir(xml_know, "isaac", root);
  const auto *xn = by_folder(modsX, "XmlNoMeta");
  require(xn == nullptr,
          "xml folder without metadata.xml and no valid content is NOT listed");

  // Folder with metadata.xml: parsed normally, not no_metadata, content valid.
  fs::create_directories(root / "XmlMod");
  write_file(root / "XmlMod" / "metadata.xml",
             "<mod><name>My Xml Mod</name><version>1.0</version></mod>");
  const auto modsX2 = engine::ModScanner::scan_dir(xml_know, "isaac", root);
  const auto *xm = by_folder(modsX2, "XmlMod");
  require(xm != nullptr && !xm->no_metadata,
          "xml mod with metadata listed, has metadata");
  require(xm->display_name == "My Xml Mod", "xml name parsed");
  require(!xm->invalid_data, "metadata presence makes content valid");

  // resources-dlc3/ with a plugin file (no metadata.xml) is valid content,
  // still no_metadata.
  fs::create_directories(root / "XmlResOnly");
  fs::create_directories(root / "XmlResOnly" / "resources-dlc3");
  write_file(root / "XmlResOnly" / "modfile.esm", "");
  const auto modsX3 = engine::ModScanner::scan_dir(xml_know, "isaac", root);
  const auto *xr = by_folder(modsX3, "XmlResOnly");
  require(xr != nullptr && !xr->invalid_data,
          "resources-dlc3/ folder with .esm is valid content");
  require(xr->no_metadata, "resources-dlc3/ folder still flagged no_metadata");

  // MO2 "Ignore missing data" persists [General] validated=true in the
  // folder's meta.ini (the scanner reads the key back). For XML games the
  // meta.ini is GMM-owned, so it suppresses both flags on a folder that has
  // neither metadata.xml nor resources.
  fs::create_directories(root / "XmlValidated");
  write_file(root / "XmlValidated" / "meta.ini",
             "[General]\nvalidated = true\n");
  const auto modsV = engine::ModScanner::scan_dir(xml_know, "isaac", root);
  const auto *xv = by_folder(modsV, "XmlValidated");
  require(xv != nullptr && !xv->invalid_data,
          "validated=true suppresses the invalid flag");
  require(!xv->no_metadata, "validated=true suppresses the no_metadata flag");

  // mark_validated() round-trip: writes the key MO2-style into meta.ini
  // (creating it), clearing the flags on rescan.
  fs::create_directories(root / "MarkedValid");
  require(engine::ModScanner::mark_validated(root / "MarkedValid"),
          "mark_validated writes meta.ini");
  {
    std::ifstream f(root / "MarkedValid" / "meta.ini");
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    require(content.find("validated") != std::string::npos,
            "mark_validated persisted 'validated' in meta.ini");
  }
  const auto modsV2 = engine::ModScanner::scan_dir(checker, "skyrim", root);
  const auto *mv = by_folder(modsV2, "MarkedValid");
  require(mv != nullptr && !mv->no_metadata,
          "mark_validated clears the no_metadata flag");
  require(!mv->invalid_data, "mark_validated clears the invalid flag");

  // Timestamps (P8.4): every real folder reports its birth (installation) and
  // mtime (changed) so the mod list can show both columns.
  const auto *timed = by_folder(modsV2, "SomeMod");
  require(timed != nullptr, "SomeMod found for timestamp check");
  require(timed->install_time > 0, "install_time populated for a real folder");
  require(timed->changed_time > 0, "changed_time populated for a real folder");
  require(timed->changed_time >= timed->install_time,
          "changed_time (mtime) is not older than install_time (btime)");
  const auto *sep = by_folder(modsV2, "My Mods_separator");
  require(sep != nullptr && sep->install_time > 0,
          "separators carry a birth time too");
  {
    // Rewriting a file bumps mtime; the re-scanned mod must reflect it.
    // Sleep past the current second first so the bump is observable at
    // whole-second granularity (the whole test runs in well under a second).
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const fs::path touched = root / "SomeMod" / "content.txt";
    write_file(touched, "hello\n");
    const auto modsR = engine::ModScanner::scan_dir(checker, "skyrim", root);
    const auto *re = by_folder(modsR, "SomeMod");
    require(re != nullptr && re->changed_time > timed->changed_time,
            "changed_time reflects a file write after install");
  }
}

TEST_CASE("delayed_disable_for", "[engine]") {
  // Default: no plugin declares delayed_disable -> false (Skyrim and all
  // other games keep the immediate disk-write behavior).
  engine::GameKnowledge knowledge;
  REQUIRE(!engine::delayed_disable_for(knowledge, "skyrimse"));
  REQUIRE(!engine::delayed_disable_for(knowledge, "isaac"));

  // A plugin declaring the hook with "true" opts in (Isaac's Direct mode).
  knowledge.set("isaac", "delayed_disable", "true");
  REQUIRE(engine::delayed_disable_for(knowledge, "isaac"));
  // Other games are unaffected by Isaac's declaration.
  REQUIRE(!engine::delayed_disable_for(knowledge, "skyrimse"));

  // Any value other than the exact "true" string is not opted in.
  knowledge.set("skyrimse", "delayed_disable", "1");
  REQUIRE(!engine::delayed_disable_for(knowledge, "skyrimse"));
  knowledge.set("skyrimse", "delayed_disable", "TRUE");
  REQUIRE(!engine::delayed_disable_for(knowledge, "skyrimse"));
}

// Workspace-93m: a set game_mods_dir (plugin hook or instance override) IS
// the mods folder. ModScanner::scan must use it literally - appending
// mods_subpath produced "{game_mods_dir}/mods", which does not exist.
TEST_CASE("scan uses game_mods_dir literally", "[engine]") {
  const fs::path root = "/tmp/gmm_scanner_test_gmd";
  fs::remove_all(root);
  fs::create_directories(root);

  // The literal mods dir holds a mod; "{literal}/mods" does NOT exist, so
  // any append would warn "mods directory not found" and return empty.
  const fs::path native = root / "Isaac Mods";
  fs::create_directories(native / "SomeMod");
  write_file(native / "SomeMod" / "meta.ini", "[General]\nversion = 1.0\n");

  engine::GameKnowledge isaac;
  isaac.set("isaacmac", "mods_subpath", "mods");
  isaac.set("isaacmac", "game_mods_dir", native.string());

  // Plugin hook: scanned literally, nothing appended.
  auto mods = engine::ModScanner::scan(isaac, "isaacmac", root / "install");
  require(by_folder(mods, "SomeMod") != nullptr,
          "hook dir scanned as-is (no /mods appended)");

  // Instance override beats the hook, also literal.
  const fs::path overridden = root / "Custom Mods";
  fs::create_directories(overridden / "OtherMod");
  write_file(overridden / "OtherMod" / "meta.ini",
             "[General]\nversion = 2.0\n");
  mods = engine::ModScanner::scan(isaac, "isaacmac", root / "install", {},
                                  overridden);
  require(by_folder(mods, "OtherMod") != nullptr,
          "instance override scanned as-is");

  // No hook/override: classic game_dir/mods_subpath fallback still works.
  engine::GameKnowledge skyrim;
  skyrim.set("skyrim", "mods_subpath", "Data");
  fs::create_directories(root / "install" / "Data" / "DataMod");
  write_file(root / "install" / "Data" / "DataMod" / "meta.ini",
             "[General]\nversion = 1.0\n");
  mods = engine::ModScanner::scan(skyrim, "skyrim", root / "install");
  require(by_folder(mods, "DataMod") != nullptr,
          "mods_subpath fallback unchanged when no game_mods_dir");

  fs::remove_all(root);
}
