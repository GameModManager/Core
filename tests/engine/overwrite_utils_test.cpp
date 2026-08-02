#include "engine/overwrite/overwrite_utils.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void touch(const fs::path& p) { std::ofstream(p).put('\n'); }

static void write_str(const fs::path& p, const std::string& s) {
    std::ofstream(p) << s;
}

static std::string read_str(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

static size_t count_files_recursive(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    size_t n = 0;
    fs::recursive_directory_iterator it(root, ec), end;
    while (it != end && !ec) {
        if (it->is_regular_file()) ++n;
        it.increment(ec);
    }
    return n;
}

int main() {
    using namespace engine;

    const fs::path base = fs::temp_directory_path() / "overwrite_utils_test_core";
    fs::remove_all(base);
    const fs::path trash = base / "trash";
    ::setenv("XDG_DATA_HOME", trash.string().c_str(), 1);

    // --- overwrite_to_mod_rel ------------------------------------------------
    {
        assert(overwrite_to_mod_rel("Data/ShaderCache/x", "Data") == "ShaderCache/x");
        assert(overwrite_to_mod_rel("Data/SkyUI/SkyUI_SE.bsa", "Data",
                                    true, "SkyUI") == "SkyUI_SE.bsa");
        assert(overwrite_to_mod_rel("Data/SkyUI/SkyUI_SE.bsa", "Data",
                                    true, "Other") == "SkyUI/SkyUI_SE.bsa");
        assert(overwrite_to_mod_rel("ControlMap_Custom.txt", "Data") ==
               "ControlMap_Custom.txt");
        assert(overwrite_to_mod_rel("data/meshes/x.nif", "Data") == "meshes/x.nif");
        assert(overwrite_to_mod_rel("Data", "Data").empty());
        assert(overwrite_to_mod_rel("Mods/MyMod/resources/gfx/x.png", "mods",
                                    true, "MyMod") == "resources/gfx/x.png");
        std::printf("  overwrite_to_mod_rel: OK\n");
    }

    // --- move_overwrite_to_mod (Skyrim: strip Data/, prune empties) ----------
    {
        const fs::path ow = base / "ow_move_skyrim";
        const fs::path mods = base / "mods_move_skyrim";
        const fs::path mod = mods / "NewMod";
        fs::create_directories(ow / "Data" / "sub");
        touch(ow / "Data" / "a.txt");
        touch(ow / "Data" / "sub" / "b.txt");
        touch(ow / "top.txt");

        assert(move_overwrite_to_mod(ow, mod, "Data"));
        assert(fs::exists(mod / "a.txt"));
        assert(fs::exists(mod / "sub" / "b.txt"));
        assert(fs::exists(mod / "top.txt"));
        assert(!fs::exists(mod / "Data"));  // Data/ prefix must be stripped
        assert(fs::exists(ow));             // the Overwrite dir itself stays
        assert(count_files_recursive(ow) == 0);

        fs::remove_all(mods);
        std::printf("  move_overwrite_to_mod (strip Data/): OK\n");
    }

    // --- move_overwrite_to_mod (include_mod_id, Isaac style) -----------------
    {
        const fs::path ow = base / "ow_move_isaac";
        const fs::path mods = base / "mods_move_isaac";
        const fs::path mod = mods / "MyMod";
        fs::create_directories(ow / "data" / "MyMod" / "resources" / "gfx");
        fs::create_directories(ow / "data" / "resources" / "gfx");
        touch(ow / "data" / "MyMod" / "resources" / "gfx" / "a.png");
        touch(ow / "data" / "resources" / "gfx" / "b.png");

        assert(move_overwrite_to_mod(ow, mod, "data", /*include_mod_id=*/true, "MyMod"));
        assert(fs::exists(mod / "resources" / "gfx" / "a.png"));
        assert(fs::exists(mod / "resources" / "gfx" / "b.png"));
        assert(!fs::exists(mod / "data"));
        assert(!fs::exists(mod / "MyMod"));

        fs::remove_all(mods);
        std::printf("  move_overwrite_to_mod (include_mod_id): OK\n");
    }

    // --- move_overwrite_entry_to_mod (single entry, mapping-root handling) ---
    {
        const fs::path ow = base / "ow_entry";
        const fs::path mods = base / "mods_entry";
        const fs::path mod = mods / "Target";
        fs::create_directories(ow / "Data" / "Shaders");
        fs::create_directories(ow / "Data" / "Meshes");
        touch(ow / "Data" / "Shaders" / "frag.hlsl");
        touch(ow / "Data" / "Meshes" / "a.nif");
        touch(ow / "top.txt");

        // Dragging a plain file: moved to the mod root (Data/ stripped).
        assert(move_overwrite_entry_to_mod(ow, ow / "Data" / "Shaders" / "frag.hlsl",
                                           mod, "Data"));
        assert(fs::exists(mod / "Shaders" / "frag.hlsl"));

        // Dragging a mapping-root directory ("Data"): its contents move.
        assert(move_overwrite_entry_to_mod(ow, ow / "Data", mod, "Data"));
        assert(fs::exists(mod / "Meshes" / "a.nif"));
        assert(!fs::exists(mod / "Data"));
        assert(!fs::exists(ow / "Data"));

        // A file outside overwrite is rejected.
        assert(!move_overwrite_entry_to_mod(ow, base / "outside.txt", mod, "Data"));
        fs::remove_all(mods);
        std::printf("  move_overwrite_entry_to_mod: OK\n");
    }

    // --- sync_overwrite_file (replace existing dest, remove overwrite src) ---
    {
        const fs::path ow = base / "ow_sync";
        const fs::path mods = base / "mods_sync";
        const fs::path mod = mods / "Target";
        fs::create_directories(ow / "Data" / "Shaders");
        fs::create_directories(mod);
        write_str(ow / "Data" / "Shaders" / "frag.hlsl", "overwrite");
        write_str(mod / "Shaders" / "frag.hlsl", "old");

        assert(sync_overwrite_file(ow, "Data/Shaders/frag.hlsl", mod, "Data"));
        assert(read_str(mod / "Shaders" / "frag.hlsl") == "overwrite");
        assert(!fs::exists(ow / "Data" / "Shaders" / "frag.hlsl"));
        assert(!fs::exists(ow / "Data"));  // pruned

        fs::remove_all(mods);
        std::printf("  sync_overwrite_file: OK\n");
    }

    // --- overwrite_is_empty ---------------------------------------------------
    {
        const fs::path ow = base / "ow_empty";
        assert(overwrite_is_empty(ow, "Data"));  // missing dir == empty
        fs::create_directories(ow);
        assert(overwrite_is_empty(ow, "Data"));
        touch(ow / "meta.ini");
        assert(overwrite_is_empty(ow, "Data"));  // meta.ini ignored
        fs::create_directories(ow / "Data");
        assert(overwrite_is_empty(ow, "Data"));  // empty mapping root ignored
        touch(ow / "Data" / "x.txt");
        assert(!overwrite_is_empty(ow, "Data"));
        fs::remove_all(ow);
        fs::create_directories(ow);
        touch(ow / "top.txt");
        assert(!overwrite_is_empty(ow, "Data"));
        std::printf("  overwrite_is_empty: OK\n");
    }

    // --- clear_overwrite (keeps mapping root, sends content to trash) --------
    {
        const fs::path ow = base / "ow_clear";
        fs::create_directories(ow / "Data" / "ShaderCache");
        touch(ow / "Data" / "ShaderCache" / "pipeline.cache");
        touch(ow / "Data" / "loose.esp");
        touch(ow / "top.txt");

        assert(clear_overwrite(ow, "Data"));
        assert(fs::exists(ow / "Data"));        // mapping root preserved
        assert(count_files_recursive(ow) == 0); // everything inside gone
        assert(fs::exists(trash / "Trash" / "files" / "loose.esp"));
        assert(fs::exists(trash / "Trash" / "files" / "top.txt"));
        assert(!fs::exists(trash / "Trash" / "files" / "Data"));  // dir kept

        std::printf("  clear_overwrite: OK\n");
    }

    // --- game_has_file --------------------------------------------------------
    {
        const fs::path game = base / "game";
        fs::create_directories(game / "Data");
        touch(game / "Data" / "present.ini");
        assert(game_has_file(game, "Data/present.ini"));
        assert(!game_has_file(game, "Data/missing.ini"));
        assert(!game_has_file({}, "Data/x"));
        std::printf("  game_has_file: OK\n");
    }

    // --- collect_overwrite_sync_files ----------------------------------------
    {
        const fs::path ow = base / "ow_collect";
        const fs::path mods = base / "mods_collect";
        const fs::path game = base / "game_collect";
        fs::create_directories(ow / "Data");
        fs::create_directories(mods / "mod_a");
        fs::create_directories(mods / "mod_b");
        fs::create_directories(game / "Data");

        touch(mods / "mod_a" / "texture.dds");
        touch(mods / "mod_b" / "texture.dds");   // conflict over texture.dds
        touch(ow / "Data" / "texture.dds");
        touch(ow / "Data" / "unique.txt");
        touch(game / "Data" / "unique.txt");     // owned by the game, not any mod

        const std::vector<std::pair<std::string, int>> mod_infos = {
            {"mod_a", 3}, {"mod_b", 1},
        };

        // Skyrim convention: higher priority number wins.
        auto files = collect_overwrite_sync_files(ow, mods, mod_infos, "Data",
                                                  /*conflict_reversed=*/false,
                                                  /*include_mod_id=*/false, game);
        assert(files.size() == 2);
        for (const auto& f : files) {
            if (f.overwrite_rel == "Data/texture.dds") {
                assert(f.owners.size() == 2);
                assert(f.owners[0].mod_id == "mod_a");   // pri 3 wins
                assert(f.owners[1].mod_id == "mod_b");
                assert(f.owners[0].priority == 3);
                assert(f.owners[1].priority == 1);
                assert(!f.game_has_file);
            } else if (f.overwrite_rel == "Data/unique.txt") {
                assert(f.owners.empty());       // no mod owns it
                assert(f.game_has_file);        // the game itself does
            } else {
                assert(false);
            }
        }

        // Isaac convention: lower priority number wins (and the mods_subpath
        // match is case-insensitive: overwrite has "Data", subpath "data").
        files = collect_overwrite_sync_files(ow, mods, mod_infos, "data",
                                             /*conflict_reversed=*/true,
                                             /*include_mod_id=*/false, game);
        for (const auto& f : files) {
            if (f.overwrite_rel == "Data/texture.dds") {
                assert(f.owners[0].mod_id == "mod_b");  // pri 1 wins
                assert(f.owners[1].mod_id == "mod_a");
            }
        }

        // include_mod_id (Isaac): deployed path is
        // "<mods_subpath>/<mod_folder>/<rel>", so the owner lookup must strip
        // the mod-folder segment.
        fs::remove_all(mods);
        fs::remove_all(ow);
        fs::create_directories(ow / "Mods" / "mod_a" / "resources" / "gfx");
        fs::create_directories(mods / "mod_a" / "resources" / "gfx");
        touch(mods / "mod_a" / "resources" / "gfx" / "a.png");
        touch(ow / "Mods" / "mod_a" / "resources" / "gfx" / "a.png");

        files = collect_overwrite_sync_files(ow, mods, mod_infos, "mods",
                                             /*conflict_reversed=*/true,
                                             /*include_mod_id=*/true);
        assert(files.size() == 1);
        assert(files[0].overwrite_rel == "Mods/mod_a/resources/gfx/a.png");
        assert(files[0].owners.size() == 1);
        assert(files[0].owners[0].mod_id == "mod_a");

        fs::remove_all(mods);
        std::printf("  collect_overwrite_sync_files: OK\n");
    }

    // --- apply_sync_plan -------------------------------------------------------
    {
        const fs::path ow = base / "ow_plan";
        const fs::path mods = base / "mods_plan";
        fs::create_directories(ow / "Data");
        touch(ow / "Data" / "x.txt");
        touch(ow / "Data" / "y.txt");
        touch(ow / "Data" / "z.txt");
        fs::create_directories(mods / "ExistingMod");
        write_str(mods / "ExistingMod" / "meta.ini", "keep-me");

        std::vector<OverwriteSyncTarget> plan = {
            {"Data/x.txt", "NewMod"},
            {"Data/y.txt", "NewMod"},
            {"Data/z.txt", "ExistingMod"},
        };
        auto moved = apply_sync_plan(plan, ow, mods, "Data", "meta.ini");
        assert(moved == 3);

        // NewMod: files moved (Data/ stripped) and meta.ini written once.
        assert(fs::exists(mods / "NewMod" / "x.txt"));
        assert(fs::exists(mods / "NewMod" / "y.txt"));
        assert(!fs::exists(mods / "NewMod" / "Data"));
        assert(fs::exists(mods / "NewMod" / "meta.ini"));
        assert(count_files_recursive(mods / "NewMod") == 3);
        assert(read_str(mods / "ExistingMod" / "meta.ini") == "keep-me");
        assert(fs::exists(mods / "ExistingMod" / "z.txt"));
        assert(count_files_recursive(mods / "ExistingMod") == 2);
        // All overwrite files consumed.
        assert(count_files_recursive(ow) == 0);

        fs::remove_all(mods);
        std::printf("  apply_sync_plan: OK\n");

        // apply_sync_plan with include_mod_id (Isaac): the mod-folder segment
        // is stripped from the destination path.
        {
            const fs::path ow2 = base / "ow_plan_isaac";
            const fs::path mods2 = base / "mods_plan_isaac";
            fs::create_directories(ow2 / "Mods" / "MyMod" / "resources" / "gfx");
            touch(ow2 / "Mods" / "MyMod" / "resources" / "gfx" / "x.png");

            std::vector<OverwriteSyncTarget> plan2 = {
                {"Mods/MyMod/resources/gfx/x.png", "MyMod"},
            };
            auto moved2 = apply_sync_plan(plan2, ow2, mods2, "mods", "meta.ini",
                                          /*include_mod_id=*/true);
            assert(moved2 == 1);
            assert(fs::exists(mods2 / "MyMod" / "resources" / "gfx" / "x.png"));
            assert(!fs::exists(mods2 / "MyMod" / "Mods"));
            assert(fs::exists(mods2 / "MyMod" / "meta.ini"));
            assert(count_files_recursive(ow2) == 0);

            fs::remove_all(mods2);
            std::printf("  apply_sync_plan (include_mod_id): OK\n");
        }
    }

    fs::remove_all(base);
    std::printf("PASS: overwrite_utils_test\n");
    return 0;
}
