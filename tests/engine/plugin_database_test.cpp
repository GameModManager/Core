// Engine test for the Skyrim plugin database (MO2 PluginList port).
//
// Part A (always runs): synthetic fixture - TES4 headers with MAST records,
// topological sort by masters with mod-priority tiebreak, Skyrim.ccc
// force-loading, transitive enable, disable blocking, profile round-trip,
// plugins.txt/loadorder.txt output, and the per-instance launch target.
//
// Part B (when the real Skyrim SE install is present): validates discovery of
// the actual plugins on disk (native ESMs first, CC flagged, SkyUI owned by
// its mod, enabled plugins.txt output).
#include "engine/instance/instance.h"
#include "engine/meta/mod_meta.h"
#include "engine/plugins/plugin_database.h"
#include "engine/plugins/esp_header.h"
#include "engine/registry/game_knowledge.h"
#include "platform/platform_interface.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void require(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

// --- Fake platform for launch-target resolution ---
class StubPlatform : public engine::PlatformInterface {
public:
    std::string platform_name() const override { return "test"; }
    fs::path data_dir() const override { return "/tmp/gmm_plugin_test_data"; }
    fs::path config_dir() const override { return "/tmp/gmm_plugin_test_config"; }
    fs::path cache_dir() const override { return "/tmp/gmm_plugin_test_cache"; }
    fs::path find_steam_root() const override { return {}; }
    bool launch_executable(const fs::path&,
                           const std::vector<std::string>&) const override {
        return false;
    }
    fs::path local_appdata;  // overrides game_local_appdata_dir
    fs::path game_local_appdata_dir(uint32_t) const override { return local_appdata; }
};

namespace {

void append_u16(std::vector<char>& v, uint16_t x) {
    v.push_back(static_cast<char>(x & 0xFF));
    v.push_back(static_cast<char>((x >> 8) & 0xFF));
}

// Minimal valid TES4 record: header + MAST subrecords (2-byte sizes).
void write_esp(const fs::path& path, bool esm_flag,
               const std::vector<std::string>& masters) {
    std::vector<char> body;  // subrecords only
    for (const auto& m : masters) {
        body.push_back('M');
        body.push_back('A');
        body.push_back('S');
        body.push_back('T');
        append_u16(body, static_cast<uint16_t>(m.size() + 1));
        body.insert(body.end(), m.begin(), m.end());
        body.push_back('\0');
    }
    std::ofstream out(path, std::ios::binary);
    out.write("TES4", 4);
    const uint32_t data_size = static_cast<uint32_t>(body.size());
    out.write(reinterpret_cast<const char*>(&data_size), 4);
    const uint32_t flags = esm_flag ? 1u : 0u;
    out.write(reinterpret_cast<const char*>(&flags), 4);
    const uint32_t zero = 0;
    out.write(reinterpret_cast<const char*>(&zero), 4);  // formid
    out.write(reinterpret_cast<const char*>(&zero), 4);  // timestamp
    out.write(reinterpret_cast<const char*>(&zero), 4);  // version
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string file_contents(const fs::path& p) {
    std::ifstream in(p);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Order of a named plugin within the list (first line = 0).
int order_of(const engine::PluginDatabase& db, const std::string& name) {
    const auto& ps = db.plugins();
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].name == name) return static_cast<int>(i);
    return -1;
}

void run_synthetic_fixture() {
    using engine::PluginDatabase;

    const fs::path base = "/tmp/gmm_plugin_fixture";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path game = base / "game";
    const fs::path mods = base / "mods";
    const fs::path meta = base / "meta";
    const fs::path profiles = base / "profiles";
    fs::create_directories(game / "Data", ec);
    fs::create_directories(mods, ec);
    fs::create_directories(meta, ec);

    // Game-native ESMs (declared order in game_native_plugins).
    write_esp(game / "Data" / "Skyrim.esm", true, {});
    write_esp(game / "Data" / "Update.esm", true, {"Skyrim.esm"});
    write_esp(game / "Data" / "Dawnguard.esm", true, {"Skyrim.esm", "Update.esm"});
    // CC content in Data + ccc list at game root.
    write_esp(game / "Data" / "ccBGSSSE001-Fish.esm", true, {"Skyrim.esm"});
    {
        std::ofstream ccc(game / "Skyrim.ccc");
        ccc << "ccBGSSSE001-Fish.esm\nccNotPresent.esm\n";
    }

    // Mod A (priority 3): SkyUI-like plugin depending on native masters only.
    fs::create_directories(mods / "SkyUI", ec);
    write_esp(mods / "SkyUI" / "SkyUI_SE.esp", false, {"Skyrim.esm", "Update.esm"});
    // Mod B (priority 5): a patch that depends on SkyUI (must sort after it).
    fs::create_directories(mods / "Patch", ec);
    write_esp(mods / "Patch" / "Patch.esp", false, {"Skyrim.esm", "SkyUI_SE.esp"});
    // Mod C (priority 1): standalone light plugin.
    fs::create_directories(mods / "Lights", ec);
    write_esp(mods / "Lights" / "Lights.esl", false, {"Skyrim.esm"});
    // Meta priorities for the mod tiebreak.
    for (const auto& [folder, prio] : std::vector<std::pair<const char*, int>>{
             {"SkyUI", 3}, {"Patch", 5}, {"Lights", 1}}) {
        auto m = engine::ModMeta::load(meta, folder);
        m.set_priority(prio);
        require(m.save(meta, folder), "meta priority saved");
    }

    // A data-only mod folder (no plugins) - must not break discovery.
    fs::create_directories(mods / "Textures", ec);
    {
        std::ofstream t(mods / "Textures" / "readme.txt");
        t << "hi\n";
    }

    PluginDatabase db;
    require(db.refresh(game, mods, meta, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
            "refresh on synthetic fixture");
    db.load_creation_club(game);
    db.sort_load_order();
    db.set_all_enabled();
    db.generate_mod_indexes();

    const auto& ps = db.plugins();
    const int n = static_cast<int>(ps.size());
    require(n == 7, "7 plugins discovered");

    // Native plugins come first, in declared order.
    require(order_of(db, "Skyrim.esm") == 0, "Skyrim.esm first");
    require(order_of(db, "Update.esm") == 1, "Update.esm second");
    require(order_of(db, "Dawnguard.esm") == 2, "Dawnguard.esm third");
    // CC right after natives.
    require(order_of(db, "ccBGSSSE001-Fish.esm") == 3, "CC plugin after natives");
    // Mod plugins topologically sorted: SkyUI before Patch; Lights stands alone.
    require(order_of(db, "SkyUI_SE.esp") == 4, "SkyUI after CC");
    require(order_of(db, "Patch.esp") == 5, "Patch after its master SkyUI");
    require(order_of(db, "Lights.esl") == 6, "Lights last");
    require(db.plugins()[order_of(db, "Lights.esl")].is_light, "Lights.esl is light");
    require(db.plugins()[order_of(db, "ccBGSSSE001-Fish.esm")].is_cc, "Fish is CC");
    require(db.plugins()[order_of(db, "ccBGSSSE001-Fish.esm")].force_loaded,
            "Fish force-loaded");
    require(db.plugins()[order_of(db, "SkyUI_SE.esp")].owner_mod == "SkyUI",
            "SkyUI owned by its mod");

    // All enabled (first-run default).
    for (const auto& p : ps) require(p.enabled, "all enabled by default");

    // Disabling a force-loaded native is blocked.
    std::string err;
    require(!db.set_enabled("Skyrim.esm", false, &err), "cannot disable native ESM");
    // Disabling SkyUI is blocked while Patch depends on it.
    err.clear();
    require(!db.set_enabled("SkyUI_SE.esp", false, &err), "cannot disable needed master");
    require(contains(err, "Patch.esp"), "error names the dependent");
    // Disabling Patch is fine.
    require(db.set_enabled("Patch.esp", false), "can disable Patch");

    // Transitive enable: re-enabling Patch re-enables SkyUI.
    require(db.set_enabled("Patch.esp", true), "re-enable Patch");
    require(db.plugins()[order_of(db, "SkyUI_SE.esp")].enabled,
            "master re-enabled transitively");

    // Missing master: plugin requires an absent master.
    fs::create_directories(mods / "Broken", ec);
    write_esp(mods / "Broken" / "Broken.esp", false, {"Skyrim.esm", "GoneMaster.esm"});
    {
        auto m = engine::ModMeta::load(meta, "Broken");
        m.set_priority(2);
        m.save(meta, "Broken");
    }
    PluginDatabase db2;
    require(db2.refresh(game, mods, meta, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
            "refresh with broken plugin");
    db2.load_creation_club(game);
    db2.sort_load_order();
    const auto* broken = db2.find("Broken.esp");
    require(broken != nullptr && broken->missing_master, "missing master flagged");

    // Enabling a plugin whose master isn't installed is blocked (not just
    // flagged), with an error naming the missing master.
    std::string errb;
    require(!db2.set_enabled("Broken.esp", true, &errb),
            "enable with absent master blocked");
    require(contains(errb, "GoneMaster.esm"), "error names the missing master");

    // Transitive: a plugin whose master chain hits an absent master is blocked
    // too, and the error names the chain plugin and its missing master.
    write_esp(mods / "Broken" / "BrokenChild.esp", false, {"Skyrim.esm", "Broken.esp"});
    PluginDatabase db2b;
    require(db2b.refresh(game, mods, meta, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
            "refresh with transitive broken plugin");
    db2b.load_creation_club(game);
    db2b.sort_load_order();
    errb.clear();
    require(!db2b.set_enabled("BrokenChild.esp", true, &errb),
            "transitive absent master blocked");
    require(contains(errb, "Broken.esp") && contains(errb, "GoneMaster.esm"),
            "error names the chain plugin and missing master");

    // Profile round-trip: save, flip enable state, load restores it.
    PluginDatabase db3;
    db3.refresh(game, mods, meta, "", "Skyrim.esm,Update.esm,Dawnguard.esm");
    db3.load_creation_club(game);
    db3.sort_load_order();
    db3.set_all_enabled();
    db3.save_profile(profiles, "Default");
    // Patch depends on SkyUI: direct disable is blocked, Patch first is fine.
    std::string err3;
    require(!db3.set_enabled("SkyUI_SE.esp", false, &err3), "SkyUI disable blocked by Patch");
    require(db3.set_enabled("Patch.esp", false), "disable Patch");
    require(db3.set_enabled("SkyUI_SE.esp", false), "disable SkyUI after Patch");
    require(!db3.plugins()[order_of(db3, "SkyUI_SE.esp")].enabled, "SkyUI now off");
    require(db3.load_profile(profiles, "Default"), "profile loads");
    require(db3.plugins()[order_of(db3, "SkyUI_SE.esp")].enabled,
            "profile restores enable state");

    // plugins.txt output: natives + CC excluded, * prefix for enabled.
    const fs::path out = base / "Plugins.txt";
    require(db3.write_game_plugins_txt(out), "writes plugins.txt");
    const std::string txt = file_contents(out);
    require(contains(txt, "*SkyUI_SE.esp"), "plugins.txt: *SkyUI_SE.esp");
    require(!contains(txt, "Skyrim.esm"), "plugins.txt: natives excluded");
    require(!contains(txt, "ccBGSSSE001-Fish.esm"), "plugins.txt: CC excluded");

    const fs::path lo = base / "loadorder.txt";
    require(db3.write_load_order_txt(lo), "writes loadorder.txt");
    const std::string lot = file_contents(lo);
    require(contains(lot, "Skyrim.esm") && contains(lot, "SkyUI_SE.esp") &&
            contains(lot, "ccBGSSSE001-Fish.esm"),
            "loadorder.txt lists everything");

    // move_plugin: fixed rows rejected, out-of-range clamps, drops outside the
    // user band clamped below the pinned rows, priorities/mod indexes recomputed.
    std::string errm;
    require(!db3.move_plugin(0, 5, &errm), "native row cannot be moved");
    require(contains(errm, "core plugin"), "move error mentions core plugin");
    errm.clear();
    require(!db3.move_plugin(99, 0, &errm), "out-of-range source rejected");
    const int skyui_row = order_of(db3, "SkyUI_SE.esp");
    const int lights_row = order_of(db3, "Lights.esl");
    require(skyui_row >= 0 && lights_row >= 0, "mod rows known");
    require(lights_row > skyui_row, "Lights starts below SkyUI (mod-priority sort)");
    // Drop Lights onto row 0 -> clamped to just below the fixed band.
    require(db3.move_plugin(lights_row, 0), "Lights moves up, clamped to user band");
    require(order_of(db3, "Lights.esl") == skyui_row, "Lights clamped to band top");
    // Move SkyUI down to the bottom (past Patch, Broken, BrokenChild).
    const int s = order_of(db3, "SkyUI_SE.esp");
    const int p = order_of(db3, "Patch.esp");
    const int bottom = static_cast<int>(db3.plugins().size()) - 1;
    require(s < p, "SkyUI still above Patch before the move");
    require(db3.move_plugin(s, bottom), "SkyUI moves to the bottom");
    require(order_of(db3, "SkyUI_SE.esp") == bottom, "SkyUI at the bottom");
    require(order_of(db3, "SkyUI_SE.esp") > order_of(db3, "Patch.esp"),
            "SkyUI now after Patch");
    const auto& psv = db3.plugins();
    for (int i = 0; i < static_cast<int>(psv.size()); ++i)
        require(psv[i].priority == i, "priority recomputed from row index");
    // No-op self-move.
    const int nl = order_of(db3, "Lights.esl");
    require(db3.move_plugin(nl, nl), "self-move is a no-op");

    // The moved order persists through a profile round-trip.
    db3.save_profile(profiles, "Default");
    PluginDatabase db4;
    db4.refresh(game, mods, meta, "", "Skyrim.esm,Update.esm,Dawnguard.esm");
    db4.load_creation_club(game);
    db4.sort_load_order();
    require(db4.load_profile(profiles, "Default"), "profile with moved order loads");
    require(order_of(db4, "Lights.esl") == order_of(db3, "Lights.esl"),
            "Lights order persisted");
    require(order_of(db4, "Patch.esp") == order_of(db3, "Patch.esp"),
            "Patch order persisted");
    require(order_of(db4, "SkyUI_SE.esp") == order_of(db3, "SkyUI_SE.esp"),
            "SkyUI order persisted");

    // Per-instance launch target: no override + stub platform resolves
    // <localappdata>/<localappdata_folder>/Plugins.txt; empty hook skips.
    const fs::path inst_root = base / "instance";
    fs::create_directories(inst_root, ec);
    engine::Instance inst = engine::Instance::from_root(inst_root);
    inst.set_path_override(engine::InstanceKind::Mods, mods);
    require(inst.write_toml(), "instance toml written");

    engine::GameKnowledge knowledge;
    knowledge.set("skyrim", "localappdata_folder", "Skyrim Special Edition");
    knowledge.set("skyrim", "game_native_plugins", "Skyrim.esm,Update.esm,Dawnguard.esm");
    StubPlatform platform;
    platform.local_appdata = base / "appdata";
    fs::create_directories(platform.local_appdata, ec);

    require(PluginDatabase::write_plugins_txt_for_launch(
                game, inst_root, "skyrim", 489830, knowledge, &platform),
            "launch write succeeds");
    const fs::path target =
        platform.local_appdata / "Skyrim Special Edition" / "Plugins.txt";
    require(fs::is_regular_file(target), "launch target written");
    const std::string lt = file_contents(target);
    require(contains(lt, "*SkyUI_SE.esp"), "launch plugins.txt has enabled SkyUI");

    // No localappdata_folder hook -> games without plugin support skip silently.
    engine::GameKnowledge no_plugins;
    require(!PluginDatabase::write_plugins_txt_for_launch(
                game, inst_root, "isaac", 250900, no_plugins, &platform),
            "games without plugin support skip");

    std::fprintf(stderr, "plugin_database_test: synthetic fixture OK\n");
    fs::remove_all(base, ec);
}

void run_real_skyrim() {
    using engine::PluginDatabase;

    const fs::path home = std::getenv("HOME") ? std::getenv("HOME") : "/home";
    const fs::path game =
        home / ".local/share/Steam/steamapps/common/Skyrim Special Edition";
    const fs::path inst =
        home / ".local/share/GameModManager/instances/Skyrim_Special_Edition";
    std::error_code ec;
    if (!fs::is_directory(game, ec) || !fs::is_directory(inst, ec)) {
        std::fprintf(stderr, "plugin_database_test: real Skyrim not found - skipped\n");
        return;
    }

    PluginDatabase db;
    require(db.refresh(game, inst / "mods", inst / "meta", "",
                       "Skyrim.esm,Update.esm,Dawnguard.esm,HearthFires.esm,Dragonborn.esm"),
            "refresh on real Skyrim");
    db.load_creation_club(game);
    db.sort_load_order();
    db.set_all_enabled();
    db.generate_mod_indexes();

    const auto& ps = db.plugins();
    require(!ps.empty(), "plugins discovered");
    require(order_of(db, "Skyrim.esm") == 0, "Skyrim.esm first in real order");
    require(order_of(db, "SkyUI_SE.esp") >= 0, "SkyUI discovered");
    const auto* skyui = db.find("SkyUI_SE.esp");
    require(skyui && !skyui->owner_mod.empty(), "SkyUI owned by a mod");
    require(skyui->enabled, "SkyUI enabled");

    // Every game-native ESM is force-loaded and unmodifiable.
    for (const char* n : {"Skyrim.esm", "Update.esm", "Dawnguard.esm",
                          "HearthFires.esm", "Dragonborn.esm"}) {
        const auto* p = db.find(n);
        require(p && p->force_loaded, "native ESM force-loaded");
    }

    // CC plugins (present in Data) are force-loaded.
    bool saw_cc = false;
    for (const auto& p : ps) {
        if (p.is_cc) {
            require(p.force_loaded, "CC plugin force-loaded");
            saw_cc = true;
        }
    }

    // The plugins.txt we would hand the game enables the installed mods.
    const fs::path out = "/tmp/gmm_real_plugins.txt";
    require(db.write_game_plugins_txt(out), "writes real plugins.txt");
    const std::string txt = file_contents(out);
    require(contains(txt, "*SkyUI_SE.esp"), "real plugins.txt enables SkyUI");

    for (const auto& p : ps) {
        std::fprintf(stderr, "  %3d  %-32s %s%s\n", p.priority, p.name.c_str(),
                     p.is_cc ? "[CC] " : "", p.mod_index_text.c_str());
    }
    std::fprintf(stderr, "plugin_database_test: real Skyrim OK (%zu plugins, cc=%s)\n",
                 ps.size(), saw_cc ? "yes" : "no");
}

}  // namespace

int main() {
    run_synthetic_fixture();
    run_real_skyrim();
    std::fprintf(stderr, "plugin_database_test: all OK\n");
    return 0;
}
