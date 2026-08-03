// Engine regression test for separator + mod metadata scanning.
//
// Pins the MO2-parity contract:
//   - separators are detected by the folder's "<name>_separator" suffix and
//     their display name is the folder minus the suffix (ModList::getDisplayName),
//   - a separator's color comes from its meta.ini [General] color key (the same
//     file MO2's ModInfoRegular::setColor writes) and stays EMPTY when no color
//     is stored - it is never defaulted to "#888888" anymore (that fallback
//     lives in the UI model rendering),
//   - regular mods are scanned from meta.ini and carry no color.
#include "engine/detect/mod_scanner.h"
#include "engine/registry/game_knowledge.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static void require(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
        std::exit(1);
    }
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p);
    out << contents;
    require(out.good(), "write_file failed for " + p.string());
}

static const engine::ScannedMod* by_folder(const std::vector<engine::ScannedMod>& mods,
                                           const std::string& folder) {
    for (const auto& m : mods)
        if (m.folder_name == folder) return &m;
    return nullptr;
}

int main() {
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

    const auto* colored = by_folder(mods, "My Mods_separator");
    require(colored != nullptr, "colored separator found");
    require(colored->is_separator, "My Mods_separator is a separator");
    require(colored->display_name == "My Mods",
            "display name is folder minus the separator suffix");
    require(colored->separator_color == "#ff888888",
            "color read from meta.ini [General] color");

    const auto* plain = by_folder(mods, "Plain_separator");
    require(plain != nullptr, "plain separator found");
    require(plain->is_separator, "Plain_separator is a separator");
    require(plain->display_name == "Plain", "plain separator display name");
    require(plain->separator_color.empty(),
            "no color default - separator_color stays empty");

    const auto* mod = by_folder(mods, "SomeMod");
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
    const auto* fm = by_folder(mods3, "FomodMod");
    require(fm != nullptr, "fomod mod found");
    require(fm->is_fomod, "mod with [fomod] choices is flagged FOMOD");
    require(fm->version == "2.0", "fomod mod keeps its version");

    // A mod whose meta.ini has a [fomod] section but no choices key is not
    // flagged - only the persisted-choice marker counts.
    fs::create_directories(root / "EmptyFomod");
    write_file(root / "EmptyFomod" / "meta.ini",
               "[General]\n[fomod]\nalwaysRestore = 1\n");
    const auto mods4 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
    const auto* ef = by_folder(mods4, "EmptyFomod");
    require(ef != nullptr, "empty-fomod mod found");
    require(!ef->is_fomod, "[fomod] without choices is not flagged FOMOD");

    // A separator whose meta.ini exists but has no color key.
    fs::create_directories(root / "NoColor_separator");
    write_file(root / "NoColor_separator" / "meta.ini", "[General]\n");
    const auto mods2 = engine::ModScanner::scan_dir(knowledge, "testgame", root);
    const auto* nc = by_folder(mods2, "NoColor_separator");
    require(nc != nullptr, "no-color separator found");
    require(nc->separator_color.empty(),
            "meta.ini without a color key yields an empty color");

    std::printf("scanner_test: all checks passed\n");
    return 0;
}
