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
#include "engine/core/instance/instance.h"
#include "engine/mod/meta/mod_meta.h"
#include "engine/pipeline/plugin_host/diagnostics_registry.h"
#include "engine/game/plugins/plugin_database.h"
#include "engine/game/plugins/esp_header.h"
#include "engine/game/registry/game_knowledge.h"
#include "platform/platform_interface.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void require(bool cond, const char* msg) {
    INFO(msg);
    REQUIRE(cond);
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
// Optional HEDR/CNAM/SNAM subrecords + record-header form version let tests
// exercise the tooltip metadata parsing (author, description, header version,
// record count / dummy detection, form version).
void write_esp(const fs::path& path, bool esm_flag,
               const std::vector<std::string>& masters,
               const std::string& author = {},
               const std::string& description = {},
               float header_version = 0.0f,
               uint32_t num_records = 0,
               uint32_t form_version = 0) {
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
    if (header_version > 0.0f || num_records > 0) {
        body.push_back('H');
        body.push_back('E');
        body.push_back('D');
        body.push_back('R');
        append_u16(body, 12);
        const char* hv = reinterpret_cast<const char*>(&header_version);
        body.insert(body.end(), hv, hv + 4);
        append_u16(body, static_cast<uint16_t>(num_records & 0xFFFF));
        append_u16(body, static_cast<uint16_t>((num_records >> 16) & 0xFFFF));
        const uint16_t next_obj = 0;
        body.insert(body.end(), reinterpret_cast<const char*>(&next_obj),
                    reinterpret_cast<const char*>(&next_obj) + 2);
        body.insert(body.end(), reinterpret_cast<const char*>(&next_obj),
                    reinterpret_cast<const char*>(&next_obj) + 2);
    }
    if (!author.empty()) {
        body.push_back('C');
        body.push_back('N');
        body.push_back('A');
        body.push_back('M');
        append_u16(body, static_cast<uint16_t>(author.size() + 1));
        body.insert(body.end(), author.begin(), author.end());
        body.push_back('\0');
    }
    if (!description.empty()) {
        body.push_back('S');
        body.push_back('N');
        body.push_back('A');
        body.push_back('M');
        append_u16(body, static_cast<uint16_t>(description.size() + 1));
        body.insert(body.end(), description.begin(), description.end());
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
    out.write(reinterpret_cast<const char*>(&form_version), 4);  // version stamp
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

// Fake ABI diagnostics providers (GmmDiagnosticsFn signature). Write zero or
// more NUL-terminated messages into the buffer.
void fake_diagnostics(const char* name, char* out, size_t cap, void*) {
    const std::string s = std::string(name) == "Alpha.esp"
                              ? std::string("hello-alpha\0bye-alpha", 22)
                              : std::string("hello-beta\0", 11);
    if (s.size() < cap) std::memcpy(out, s.data(), s.size());
}

void fake_diagnostics_other(const char*, char* out, size_t cap, void*) {
    const std::string s("othergame-msg\0", 14);
    if (s.size() < cap) std::memcpy(out, s.data(), s.size());
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
    // Carries full TES4 header metadata (author/description/HEDR/form version)
    // plus same-origin INI + archive files for the tooltip detection.
    fs::create_directories(mods / "SkyUI", ec);
    write_esp(mods / "SkyUI" / "SkyUI_SE.esp", false, {"Skyrim.esm", "Update.esm"},
              "SkyUI team", "Sky UI", 1.70f, 100, 44);
    {
        std::ofstream ini(mods / "SkyUI" / "SkyUI_SE.ini");
        ini << "[General]\n";
        std::ofstream bsa(mods / "SkyUI" / "SkyUI_SE.bsa", std::ios::binary);
        bsa.write("BSA", 3);
        std::ofstream bsa2(mods / "SkyUI" / "SkyUI_SE - Textures.bsa", std::ios::binary);
        bsa2.write("BSA", 3);
        // Unrelated archive must NOT be associated with SkyUI (prefix rule).
        std::ofstream other(mods / "SkyUI" / "OtherStuff.bsa", std::ios::binary);
        other.write("BSA", 3);
    }
    // Mod B (priority 5): a patch that depends on SkyUI (must sort after it).
    fs::create_directories(mods / "Patch", ec);
    write_esp(mods / "Patch" / "Patch.esp", false, {"Skyrim.esm", "SkyUI_SE.esp"});
    // Mod C (priority 1): standalone light plugin, zero records (dummy).
    fs::create_directories(mods / "Lights", ec);
    write_esp(mods / "Lights" / "Lights.esl", false, {"Skyrim.esm"},
              "", "", 1.70f, 0, 44);
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
    require(db.plugins()[order_of(db, "Lights.esl")].is_light(), "Lights.esl is light");
    require(db.plugins()[order_of(db, "ccBGSSSE001-Fish.esm")].is_cc, "Fish is CC");
    require(db.plugins()[order_of(db, "ccBGSSSE001-Fish.esm")].force_loaded,
            "Fish force-loaded");
    require(db.plugins()[order_of(db, "SkyUI_SE.esp")].owner_mod == "SkyUI",
            "SkyUI owned by its mod");

    // --- Apply a LOOT-style sorted order (PLAN.md §7.1) ---
    {
        // Lock SkyUI at its current priority first: an auto-sort must never
        // move it.
        require(db.set_locked("SkyUI_SE.esp", true), "SkyUI locked");

        // Feed a deliberately shuffled user order. The native + CC band must
        // stay fixed; the user band follows the requested order.
        const std::vector<std::string> loot_order = {
            "Lights.esl", "Patch.esp", "SkyUI_SE.esp"};
        require(db.apply_load_order(loot_order), "apply LOOT order");
        require(db.plugins()[0].name == "Skyrim.esm", "native band first");
        require(order_of(db, "ccBGSSSE001-Fish.esm") == 3, "CC band fixed");
        // SkyUI is locked at priority 4, so the requested order applies to the
        // slots above it: SkyUI claims 4 (auto-sort could not move it) and
        // Lights/Patch shift down into 5/6.
        require(order_of(db, "SkyUI_SE.esp") == 4, "locked SkyUI stays at 4");
        require(order_of(db, "Lights.esl") == 5, "Lights follows the locked row");
        require(order_of(db, "Patch.esp") == 6, "Patch follows Lights");
        // Mod indexes were regenerated for the new order (Lights keeps its
        // light slot; Patch, last in the full-index band, still reads 05).
        require(db.plugins()[order_of(db, "Lights.esl")].mod_index == 0xFE000000u,
                "light index regenerated after apply");
        require(db.plugins()[order_of(db, "Patch.esp")].mod_index == 5u,
                "full index regenerated after apply");

        // An unknown name must fail without touching the order.
        const auto before = db.plugins();
        std::string err;
        require(!db.apply_load_order({"Lights.esl", "NotARealPlugin.esp"}, &err),
                "unknown plugin rejected");
        require(err.find("NotARealPlugin.esp") != std::string::npos,
                "error names the unknown plugin");
        require(db.plugins().size() == before.size(), "apply failure is a no-op");
        require(order_of(db, "Lights.esl") == 5, "order unchanged after failure");

        // Unlock so later fixture blocks start from a clean state.
        require(db.set_locked("SkyUI_SE.esp", false), "SkyUI unlocked");
    }

    // --- TES4 header metadata (tooltip parity) ---
    {
        const auto& skyui = db.plugins()[order_of(db, "SkyUI_SE.esp")];
        require(skyui.author == "SkyUI team", "author parsed from CNAM");
        require(skyui.description == "Sky UI", "description parsed from SNAM");
        require(skyui.header_version == 1.70f, "header version parsed from HEDR");
        require(skyui.form_version == 44, "form version parsed from version stamp");
        require(!skyui.has_no_records, "100-record plugin not marked dummy");

        const auto& native = db.plugins()[order_of(db, "Skyrim.esm")];
        require(native.form_version == 0 && native.author.empty(),
                "no-metadata plugin keeps zeroed fields");

        const auto& lights = db.plugins()[order_of(db, "Lights.esl")];
        require(lights.has_no_records, "zero-record plugin marked dummy");
    }

    // --- Same-origin assets (Loads Archives / Loads INI) ---
    {
        const auto& skyui = db.plugins()[order_of(db, "SkyUI_SE.esp")];
        require(skyui.has_ini, "SkyUI.ini detected");
        require(skyui.archives.size() == 2 &&
                    skyui.archives[0] == "SkyUI_SE - Textures.bsa" &&
                    skyui.archives[1] == "SkyUI_SE.bsa",
                "archives match by basename prefix, sorted, unrelated excluded");
        const auto& native = db.plugins()[order_of(db, "Skyrim.esm")];
        require(!native.has_ini && native.archives.empty(),
                "no assets for game-native plugin");
    }

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
    require(broken->missing_masters.size() == 1 &&
                broken->missing_masters[0] == "GoneMaster.esm",
            "missing master names recorded");

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

    // Case-insensitive master resolution. Header MAST strings are byte-exact
    // ("skyrim.esm", "caselib.esp") while plugin names come from on-disk
    // filenames (Skyrim.esm, CaseLib.esp). Windows games resolve these on a
    // case-insensitive filesystem, so the lookup must match ignoring case -
    // otherwise real masters are falsely flagged and dependency ordering breaks.
    fs::create_directories(mods / "CaseMod", ec);
    write_esp(mods / "CaseMod" / "CasePlugin.esp", false, {"skyrim.esm"});
    fs::create_directories(mods / "CaseLib", ec);
    write_esp(mods / "CaseLib" / "CaseLib.esp", false, {"Skyrim.esm"});
    fs::create_directories(mods / "CaseClient", ec);
    write_esp(mods / "CaseClient" / "CaseClient.esp", false, {"caselib.esp"});
    PluginDatabase dbc;
    require(dbc.refresh(game, mods, meta, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
            "refresh with case-mismatched masters");
    dbc.load_creation_club(game);
    dbc.sort_load_order();
    dbc.set_all_enabled();
    require(order_of(dbc, "CaseLib.esp") < order_of(dbc, "CaseClient.esp"),
            "case-mismatched master still orders dependent after master");
    const auto* casep = dbc.find("CasePlugin.esp");
    require(casep && !casep->missing_master,
            "lowercase master flag resolved against Skyrim.esm");
    // Disabling CaseLib is blocked: CaseClient requires it via "caselib.esp".
    std::string ercc;
    require(!dbc.set_enabled("CaseLib.esp", false, &ercc),
            "case-insensitive disable block");
    require(contains(ercc, "CaseClient.esp"), "disable error names the dependent");
    // Re-enabling a plugin whose lowercase master resolves works.
    require(dbc.set_enabled("CaseClient.esp", false), "disable CaseClient");
    ercc.clear();
    require(dbc.set_enabled("CaseClient.esp", true, &ercc),
            "re-enable CaseClient with case-resolved master");

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

    // Locked plugins: pinned, immovable, sort-proof, and persisted through a
    // profile round-trip (MO2 lockedorder.txt parity).
    {
        const fs::path b = base / "lock";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        const fs::path pf = b / "profiles";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        fs::create_directories(pf, ec);
        write_esp(g / "Data" / "Skyrim.esm", true, {});
        write_esp(g / "Data" / "Update.esm", true, {"Skyrim.esm"});
        fs::create_directories(m / "ModA", ec);
        write_esp(m / "ModA" / "ModA.esp", false, {"Skyrim.esm"});
        fs::create_directories(m / "ModB", ec);
        write_esp(m / "ModB" / "ModB.esp", false, {"Skyrim.esm"});
        fs::create_directories(m / "ModC", ec);
        write_esp(m / "ModC" / "ModC.esp", false, {"Skyrim.esm"});

        PluginDatabase dbl;
        require(dbl.refresh(g, m, mt, "", "Skyrim.esm,Update.esm"),
                "refresh lock fixture");
        dbl.sort_load_order();
        dbl.set_all_enabled();
        require(order_of(dbl, "ModA.esp") == 2 && order_of(dbl, "ModB.esp") == 3 &&
                    order_of(dbl, "ModC.esp") == 4,
                "lock fixture starts sorted by name");

        // Locking a core plugin is refused.
        std::string errl;
        require(!dbl.set_locked("Skyrim.esm", true, &errl),
                "core plugin cannot be locked");
        require(contains(errl, "core plugin"), "lock error mentions core plugin");

        // Lock ModB at its current row.
        const int b_row = order_of(dbl, "ModB.esp");
        require(dbl.set_locked("ModB.esp", true), "ModB locks");
        require(dbl.is_locked("ModB.esp"), "ModB is locked");
        require(dbl.plugins()[b_row].locked, "ModB locked flag set");

        // A locked plugin cannot be moved by drag.
        errl.clear();
        require(!dbl.move_plugin(b_row, 0, &errl), "locked plugin cannot be moved");
        require(contains(errl, "locked"), "move error mentions locked");

        // A drop onto the locked row would displace it - rejected.
        errl.clear();
        const int a_row = order_of(dbl, "ModA.esp");
        require(!dbl.move_plugin(a_row, b_row, &errl),
                "cannot drop onto a locked row");
        require(contains(errl, "displaced"), "drop error mentions displacement");

        // Sort keeps the pin.
        dbl.sort_load_order();
        require(dbl.is_locked("ModB.esp"), "lock survives a sort");
        require(order_of(dbl, "ModB.esp") == b_row,
                "locked plugin keeps its row through a sort");

        // Persistence: lockedorder.txt records it, and a fresh DB re-pins it.
        dbl.save_profile(pf, "Default");
        const std::string lockfile =
            file_contents(pf / "Default" / "lockedorder.txt");
        require(contains(lockfile, "ModB.esp|" + std::to_string(b_row)),
                "lockedorder.txt records ModB|row");

        PluginDatabase dbk;
        require(dbk.refresh(g, m, mt, "", "Skyrim.esm,Update.esm"),
                "refresh lock reload");
        dbk.sort_load_order();
        require(dbk.load_profile(pf, "Default"), "lock profile loads");
        require(dbk.is_locked("ModB.esp"), "lock restored from lockedorder.txt");
        require(order_of(dbk, "ModB.esp") == b_row,
                "locked plugin restored at its row");
        require(!dbk.is_locked("ModA.esp"), "unlocked plugin stays unlocked");

        // A hand-written lockedorder.txt pin also sticks (MO2
        // readLockedOrderFrom parity), and an unlocked plugin stays free.
        {
            std::ofstream lo(pf / "Default" / "lockedorder.txt");
            lo << "# a comment\nModC.esp|" << (b_row + 1) << "\n";
        }
        PluginDatabase dbm;
        require(dbm.refresh(g, m, mt, "", "Skyrim.esm,Update.esm"),
                "refresh lock handwrite");
        dbm.sort_load_order();
        require(dbm.load_profile(pf, "Default"), "hand-written lock loads");
        require(dbm.is_locked("ModC.esp"), "hand-written pin sticks");
        require(!dbm.is_locked("ModB.esp"), "unlisted plugin loads unlocked");

        // Unlock clears the pin and drops it from lockedorder.txt.
        errl.clear();
        require(dbl.set_locked("ModB.esp", false), "ModB unlocks");
        require(!dbl.is_locked("ModB.esp"), "ModB no longer locked");
        require(!dbl.plugins()[order_of(dbl, "ModB.esp")].locked,
                "locked flag cleared");
        dbl.save_profile(pf, "Default");
        require(!contains(file_contents(pf / "Default" / "lockedorder.txt"),
                          "ModB.esp|"),
                "unlocked plugin removed from lockedorder.txt");
    }

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

    // Band invariant: re-cased game-native ESMs stay force-loaded in the fixed
    // band above user plugins, and cc-prefixed content needs no Skyrim.ccc.
    {
        const fs::path b = base / "bandA";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        // On-disk names re-cased vs the declared canonical list.
        write_esp(g / "Data" / "skyrim.esm", true, {});
        write_esp(g / "Data" / "update.esm", true, {"Skyrim.esm"});
        // CC content detected by prefix alone (no Skyrim.ccc file).
        write_esp(g / "Data" / "ccBGSFO4001-Whatever.esm", true, {"Skyrim.esm"});
        fs::create_directories(m / "UserMod", ec);
        write_esp(m / "UserMod" / "UserMod.esp", false, {"skyrim.esm", "update.esm"});

        PluginDatabase dba;
        require(dba.refresh(g, m, mt, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
                "refresh re-cased natives");
        dba.sort_load_order();
        dba.set_all_enabled();
        dba.generate_mod_indexes();
        require(order_of(dba, "skyrim.esm") == 0, "re-cased native keeps declared slot");
        require(order_of(dba, "update.esm") == 1, "re-cased native keeps declared slot 2");
        require(dba.plugins()[0].is_game_native && dba.plugins()[0].force_loaded,
                "re-cased native force-loaded");
        require(dba.plugins()[1].is_game_native && dba.plugins()[1].force_loaded,
                "re-cased native 2 force-loaded");
        const int cc_row = order_of(dba, "ccBGSFO4001-Whatever.esm");
        require(cc_row == 2, "cc-prefix plugin in CC band without Skyrim.ccc");
        require(dba.plugins()[cc_row].is_cc, "cc-prefix flagged CC");
        require(dba.plugins()[cc_row].force_loaded, "cc-prefix force-loaded");
        const int user_row = order_of(dba, "UserMod.esp");
        require(user_row == 3 && user_row > cc_row, "user plugin after the fixed band");
        std::string erra;
        require(!dba.move_plugin(0, static_cast<int>(dba.plugins().size()) - 1, &erra),
                "re-cased native cannot be moved to the bottom");
        require(contains(erra, "core plugin"), "move error mentions core plugin");
    }

    // Band heal: a stale loadorder.txt that parks core plugins at the bottom
    // is repaired on load (repaired=true, natives back on top, user order
    // preserved) and the corrected order is persisted immediately.
    {
        const fs::path b = base / "bandB";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        const fs::path pf = b / "profiles";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        fs::create_directories(pf, ec);
        write_esp(g / "Data" / "Skyrim.esm", true, {});
        write_esp(g / "Data" / "Update.esm", true, {"Skyrim.esm"});
        write_esp(g / "Data" / "Dawnguard.esm", true, {"Skyrim.esm", "Update.esm"});
        fs::create_directories(m / "UserMod", ec);
        write_esp(m / "UserMod" / "UserMod.esp", false, {"Skyrim.esm"});
        fs::create_directories(m / "Another", ec);
        write_esp(m / "Another" / "Another.esp", false, {"Skyrim.esm"});

        PluginDatabase dbb;
        require(dbb.refresh(g, m, mt, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
                "refresh heal fixture");
        dbb.sort_load_order();
        dbb.set_all_enabled();
        dbb.save_profile(pf, "Default");
        require(order_of(dbb, "Skyrim.esm") == 0, "sorted natives first");

        // Corrupt the persisted order: core plugins parked at the bottom.
        {
            std::ofstream lo(pf / "Default" / "loadorder.txt");
            lo << "UserMod.esp\nAnother.esp\nUpdate.esm\nDawnguard.esm\nSkyrim.esm\n";
        }
        PluginDatabase dbc2;
        require(dbc2.refresh(g, m, mt, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
                "refresh heal fixture again");
        dbc2.sort_load_order();
        bool repaired = false;
        require(dbc2.load_profile(pf, "Default", &repaired), "profile loads");
        require(repaired, "broken core order repaired on load");
        require(order_of(dbc2, "Skyrim.esm") < order_of(dbc2, "UserMod.esp"),
                "native healed above user plugins");
        require(order_of(dbc2, "UserMod.esp") < order_of(dbc2, "Another.esp"),
                "user relative order preserved");
        require(order_of(dbc2, "Skyrim.esm") == 0 && order_of(dbc2, "Update.esm") == 1 &&
                order_of(dbc2, "Dawnguard.esm") == 2,
                "declared native order restored");
        dbc2.save_profile(pf, "Default");  // persist-immediately
        const std::string healed = file_contents(pf / "Default" / "loadorder.txt");
        const auto first_native = healed.find("Skyrim.esm\n");
        require(first_native != std::string::npos &&
                first_native < healed.find("UserMod.esp\n") &&
                healed.find("Update.esm\n") < healed.find("UserMod.esp\n"),
                "healed order persisted to loadorder.txt");

        // A clean profile loads without repair.
        PluginDatabase dbc3;
        require(dbc3.refresh(g, m, mt, "", "Skyrim.esm,Update.esm,Dawnguard.esm"),
                "refresh heal fixture for clean load");
        dbc3.sort_load_order();
        bool repaired2 = true;
        require(dbc3.load_profile(pf, "Default", &repaired2), "clean profile loads");
        require(!repaired2, "clean profile does not need repair");
    }

    // Newly installed mods: a plugin that appears after the profile was saved
    // is not in plugins.txt, so it must default to enabled when its masters are
    // present AND active (MO2 parity) - and stay disabled when a master is
    // absent or was disabled by the user.
    {
        const fs::path b = base / "newplug";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        const fs::path pf = b / "profiles";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        fs::create_directories(pf / "Default", ec);
        write_esp(g / "Data" / "Skyrim.esm", true, {});
        write_esp(g / "Data" / "Update.esm", true, {"Skyrim.esm"});

        // Installed before the profile existed; the saved plugins.txt disables it.
        fs::create_directories(m / "OldMod", ec);
        write_esp(m / "OldMod" / "OldPlugin.esp", false, {"Skyrim.esm"});
        {
            std::ofstream pt(pf / "Default" / "plugins.txt");
            pt << "OldPlugin.esp\n";  // listed without '*', i.e. disabled
        }
        {
            std::ofstream lo(pf / "Default" / "loadorder.txt");
            lo << "OldPlugin.esp\n";
        }

        // Installed after the profile existed.
        fs::create_directories(m / "NewMod", ec);
        write_esp(m / "NewMod" / "NewPlugin.esp", false, {"Skyrim.esm", "Update.esm"});
        fs::create_directories(m / "Gated", ec);
        write_esp(m / "Gated" / "GatedPlugin.esp", false, {"Skyrim.esm", "OldPlugin.esp"});
        fs::create_directories(m / "NoMaster", ec);
        write_esp(m / "NoMaster" / "NoMaster.esp", false, {"Skyrim.esm", "AbsentMaster.esm"});
        fs::create_directories(m / "Chain", ec);
        write_esp(m / "Chain" / "ChainPlugin.esp", false, {"Skyrim.esm", "NewPlugin.esp"});

        PluginDatabase dbn;
        require(dbn.refresh(g, m, mt, "", "Skyrim.esm,Update.esm"),
                "refresh new-plugin fixture");
        dbn.load_creation_club(g);
        dbn.sort_load_order();
        require(dbn.load_profile(pf, "Default"), "profile with new plugins loads");
        require(!dbn.plugins()[order_of(dbn, "OldPlugin.esp")].enabled,
                "persisted disabled plugin stays disabled");
        require(dbn.plugins()[order_of(dbn, "NewPlugin.esp")].enabled,
                "new plugin enabled by default (masters active)");
        require(dbn.plugins()[order_of(dbn, "ChainPlugin.esp")].enabled,
                "new plugin enabled transitively via a new enabled master");
        require(!dbn.plugins()[order_of(dbn, "GatedPlugin.esp")].enabled,
                "new plugin gated on a user-disabled master stays disabled");
        require(!dbn.plugins()[order_of(dbn, "NoMaster.esp")].enabled,
                "new plugin with an absent master stays disabled");
        require(dbn.plugins()[order_of(dbn, "NoMaster.esp")].missing_master,
                "absent-master plugin still flagged");
    }

    // No-profile bootstrap: set_all_enabled() skips plugins whose master is
    // absent (nothing to enable them against) while enabling the rest.
    {
        const fs::path b = base / "bootstrap";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        write_esp(g / "Data" / "Skyrim.esm", true, {});
        fs::create_directories(m / "Ok", ec);
        write_esp(m / "Ok" / "Ok.esp", false, {"Skyrim.esm"});
        fs::create_directories(m / "Borked", ec);
        write_esp(m / "Borked" / "Borked.esp", false, {"Skyrim.esm", "MissingMaster.esm"});

        PluginDatabase dbs;
        require(dbs.refresh(g, m, mt, "", "Skyrim.esm"), "refresh bootstrap fixture");
        dbs.load_creation_club(g);
        dbs.sort_load_order();
        dbs.set_all_enabled();
        require(dbs.plugins()[order_of(dbs, "Ok.esp")].enabled,
                "bootstrap enables plugins with present masters");
        require(!dbs.plugins()[order_of(dbs, "Borked.esp")].enabled,
                "bootstrap keeps missing-master plugin disabled");
    }

    // Diagnostics providers (ABI register_diagnostics): called once per plugin
    // after refresh, their NUL-terminated messages land in GamePlugin::messages
    // for the tooltip (MO2 addInformation parity).
    {
        engine::PluginDatabase ddx;
        auto& dxps = ddx.plugins_mutable();
        {
            engine::GamePlugin a;
            a.name = "Alpha.esp";
            engine::GamePlugin b;
            b.name = "Beta.esp";
            dxps.push_back(a);
            dxps.push_back(b);
        }

        engine::DiagnosticsRegistry::instance().clear();
        engine::DiagnosticsRegistry::instance().register_provider(
            "skyrim", fake_diagnostics, nullptr);
        engine::DiagnosticsRegistry::instance().register_provider(
            "othergame", fake_diagnostics_other, nullptr);

        engine::DiagnosticsRegistry::instance().collect("skyrim", ddx);
        require(dxps[0].messages.size() == 2 &&
                    dxps[0].messages[0] == "hello-alpha" &&
                    dxps[0].messages[1] == "bye-alpha",
                "provider messages parsed into GamePlugin::messages");
        require(dxps[1].messages.size() == 1 &&
                    dxps[1].messages[0] == "hello-beta",
                "provider runs for every plugin");
        require(dxps[1].messages[0] != "othergame-msg",
                "providers for other games are skipped");

        // collect() clears previous messages before re-running (replaces, not
        // appends) - a stale provider message never lingers across refreshes.
        engine::DiagnosticsRegistry::instance().collect("skyrim", ddx);
        require(dxps[0].messages.size() == 2 &&
                    dxps[0].messages[0] == "hello-alpha",
                "collect replaces previous messages");

        engine::DiagnosticsRegistry::instance().clear();
    }

    // Re-cased Skyrim.ccc: the ccc-file lookup is Windows-native and must find
    // "skyrim.ccc"/"Data/skyrim.ccc" regardless of on-disk casing, or CC
    // ordering/force-loading is silently skipped.
    {
        const fs::path b = base / "cccCase";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        write_esp(g / "Data" / "Skyrim.esm", true, {});
        // A non-"cc"-prefixed plugin can only be force-loaded via the ccc
        // file, so it discriminates "the file was actually found" from the
        // prefix rule.
        write_esp(g / "Data" / "ccBGSSSE001-Fish.esm", true, {"Skyrim.esm"});
        write_esp(g / "Data" / "SeasonsOfSkyrim.esm", true, {"Skyrim.esm"});
        {
            std::ofstream ccc(g / "Data" / "skyrim.ccc");
            ccc << "ccBGSSSE001-Fish.esm\nSeasonsOfSkyrim.esm\n";
        }
        PluginDatabase dcc;
        require(dcc.refresh(g, m, mt, "", "Skyrim.esm"),
                "refresh re-cased ccc fixture");
        dcc.load_creation_club(g);
        dcc.sort_load_order();
        const auto& cps = dcc.plugins();
        require(cps.size() == 3, "3 plugins in re-cased ccc fixture");
        const int seasons = order_of(dcc, "SeasonsOfSkyrim.esm");
        require(seasons >= 0, "Seasons discovered");
        require(cps[seasons].is_cc && cps[seasons].force_loaded,
                "non-cc-prefixed plugin force-loaded via re-cased Skyrim.ccc");
        require(order_of(dcc, "ccBGSSSE001-Fish.esm") < seasons,
                "ccc declared order respected");
    }

    // CI shadow dedup: a mod shipping a re-cased copy of a game-Data plugin
    // (e.g. "Skyrim.esm" over the game's "skyrim.esm") must produce ONE row,
    // not a duplicate native.
    {
        const fs::path b = base / "shadow";
        const fs::path g = b / "game";
        const fs::path m = b / "mods";
        const fs::path mt = b / "meta";
        fs::create_directories(g / "Data", ec);
        fs::create_directories(m, ec);
        fs::create_directories(mt, ec);
        write_esp(g / "Data" / "skyrim.esm", true, {});
        fs::create_directories(m / "ShadowMod", ec);
        write_esp(m / "ShadowMod" / "Skyrim.esm", true, {});
        PluginDatabase dsh;
        require(dsh.refresh(g, m, mt, "", "Skyrim.esm"),
                "refresh shadow fixture");
        dsh.sort_load_order();
        require(dsh.plugins().size() == 1,
                "re-cased mod copy shadows the game Data plugin (one row)");
        require(order_of(dsh, "Skyrim.esm") == 0,
                "mod copy wins the name conflict");
        require(dsh.plugins()[0].is_game_native && dsh.plugins()[0].force_loaded,
                "shadowing copy keeps native band membership");
    }

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

// A mod disabled via the default GMM sentinel (.gmmdisabled) must contribute
// nothing to the plugin database. Regression for the GUI passing "" as
// disable_mechanism (main_window.cpp), which silently re-enabled disabled mods.
void run_disabled_mod_fixture() {
    using engine::PluginDatabase;

    const fs::path base = "/tmp/gmm_plugin_disabled_fixture";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path game = base / "game";
    const fs::path mods = base / "mods";
    const fs::path meta = base / "meta";
    fs::create_directories(game / "Data", ec);
    fs::create_directories(mods, ec);
    fs::create_directories(meta, ec);

    write_esp(game / "Data" / "Skyrim.esm", true, {});
    fs::create_directories(mods / "EnabledMod", ec);
    write_esp(mods / "EnabledMod" / "Enabled.esp", false, {"Skyrim.esm"});
    fs::create_directories(mods / "DisabledMod", ec);
    write_esp(mods / "DisabledMod" / "Disabled.esp", false, {"Skyrim.esm"});
    std::ofstream sentinel(mods / "DisabledMod" / ".gmmdisabled");
    sentinel << "\n";

    // The disable_mechanism_for() helper: declared hook wins, else default.
    {
        engine::GameKnowledge k;
        require(engine::disable_mechanism_for(k, "anygame") == ".gmmdisabled",
                "no declaration -> engine default sentinel");
        k.set("anygame", "disable_mechanism", "disable.it");
        require(engine::disable_mechanism_for(k, "anygame") == "disable.it",
                "game-declared mechanism wins over default");
    }

    // Old buggy path: empty mechanism -> the disabled mod's plugin still counts.
    {
        PluginDatabase db;
        require(db.refresh(game, mods, meta, "", "Skyrim.esm"),
                "refresh with empty mechanism");
        require(db.find("Disabled.esp") != nullptr,
                "empty mechanism must NOT exclude (guards the regression)");
        require(db.find("Enabled.esp") != nullptr, "enabled mod present");
    }

    // Correct: the sentinel is honored -> disabled mod contributes nothing.
    {
        PluginDatabase db;
        require(db.refresh(game, mods, meta, ".gmmdisabled", "Skyrim.esm"),
                "refresh with default sentinel");
        require(db.find("Disabled.esp") == nullptr,
                "disabled mod's plugin is excluded");
        require(db.find("Enabled.esp") != nullptr, "enabled mod present");
    }

    fs::remove_all(base, ec);
    std::fprintf(stderr, "plugin_database_test: disabled-mod fixture OK\n");
}

}  // namespace

TEST_CASE("plugin database", "[engine]") {
    run_synthetic_fixture();
    run_disabled_mod_fixture();
    run_real_skyrim();
}
