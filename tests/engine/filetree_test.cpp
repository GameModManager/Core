// Engine regression test for the unified file tree (PLAN §19 P1.1).
//
// Pins the MO2 IFileTree-modeled engine/filetree module:
//
//   1. name_compare / name_equals under both case modes.
//   2. FileTreeEntry API: suffix/has_suffix, path/path_from, parent, is_dir.
//   3. Directory tree: lazy population, find/exists/find_directory (CI, '/'
//      and '\\' separators), at/size, path_to, walk CONTINUE/STOP/SKIP.
//   4. CaseSensitive mode at tree creation.
//   5. THE Phase 7 exit criterion: a synthetic archive and a mod folder with
//      identical content produce the same FileTree shape.
//   6. analyze_staging_layout runs on trees: the wrapper-peel decision on an
//      ArchiveFileTree matches a DirectoryFileTree of the same content, and
//      matches MO2 InstallerQuick::getSimpleArchiveBase semantics (incl. the
//      DataText top layer and the never-touch-FOMOD guard).
//   7. normalize_staging_root still applies the verdict physically.
//
// Uses a check() counter that returns a real non-zero exit code (Release
// builds compile asserts out under -DNDEBUG, so a bare assert() suite cannot
// fail ctest - see build/CMakeCache.txt).
#include "engine/filetree/file_tree.h"
#include "engine/filetree/dir_file_tree.h"
#include "engine/filetree/archive_file_tree.h"
#include "engine/filetree/staging_layout.h"
#include "engine/registry/game_features/mod_data_checker.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

struct TempDir {
    fs::path root;
    TempDir() {
        static int counter = 0;
        root = fs::temp_directory_path() / ("gmm_filetree_test_" +
                                            std::to_string(::getpid()) + "_" +
                                            std::to_string(counter++));
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

struct ZipItem {
    std::string path;  // '/'-separated, relative to the archive root
    std::string content;
    bool is_dir = false;
};

// Build a zip via the libarchive WRITE API so the listing backend is tested
// against a real (if synthetic) archive, not against fixture files.
static bool make_zip(const fs::path& zip_path, const std::vector<ZipItem>& items) {
    struct archive* a = archive_write_new();
    archive_write_set_format_zip(a);
    if (archive_write_open_filename(a, zip_path.c_str()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }
    bool ok = true;
    for (const auto& item : items) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, item.path.c_str());
        archive_entry_set_filetype(e, item.is_dir ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(e, item.is_dir ? 0755 : 0644);
        archive_entry_set_size(e, static_cast<la_int64_t>(item.content.size()));
        if (archive_write_header(a, e) != ARCHIVE_OK) {
            ok = false;
            archive_entry_free(e);
            break;
        }
        if (!item.is_dir && !item.content.empty() &&
            archive_write_data(a, item.content.data(), item.content.size()) < 0) {
            ok = false;
            archive_entry_free(e);
            break;
        }
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return ok;
}

static void make_dir_tree(const fs::path& root, const std::vector<ZipItem>& items) {
    for (const auto& item : items) {
        fs::path p = root / item.path;
        if (item.is_dir) {
            fs::create_directories(p);
        } else {
            fs::create_directories(p.parent_path());
            std::ofstream(p) << item.content;
        }
    }
}

struct ShapeEntry {
    std::string path;
    bool is_dir;
};

// Walk a tree into a sorted (path, is_dir) list - the shape-equivalence key.
static std::vector<ShapeEntry> collect_shape(const std::shared_ptr<const engine::FileTree>& tree) {
    std::vector<ShapeEntry> out;
    tree->walk(
        [&out](const std::string& prefix,
               const engine::FileTree::const_reference& entry) {
            out.push_back({prefix + entry->name(), entry->is_dir()});
            return engine::FileTree::WalkReturn::Continue;
        });
    std::sort(out.begin(), out.end(),
              [](const ShapeEntry& a, const ShapeEntry& b) {
                  if (a.path != b.path) return a.path < b.path;
                  return a.is_dir && !b.is_dir;
              });
    return out;
}

static void assert_same_shape(const std::shared_ptr<const engine::FileTree>& a,
                              const std::shared_ptr<const engine::FileTree>& b,
                              const char* what) {
    auto sa = collect_shape(a);
    auto sb = collect_shape(b);
    bool same = sa.size() == sb.size();
    if (same) {
        for (std::size_t i = 0; i < sa.size(); ++i) {
            if (sa[i].path != sb[i].path || sa[i].is_dir != sb[i].is_dir) {
                same = false;
                break;
            }
        }
    }
    check(same, what);
}

static bool same_verdict(const engine::StagingNormalizeResult& a,
                         const engine::StagingNormalizeResult& b) {
    return a.fomod == b.fomod && a.simple == b.simple &&
           a.merged_data_dir == b.merged_data_dir &&
           a.peeled_folder_hint == b.peeled_folder_hint &&
           a.peel_chain == b.peel_chain;
}

TEST_CASE("filetree", "[engine]") {
    using namespace engine;

    // --- 1. name comparison ------------------------------------------------
    check(name_compare("abc", "ABC", NameCompare::CaseInsensitive) == 0,
          "name_compare CI equal");
    check(name_compare("a", "b", NameCompare::CaseInsensitive) < 0,
          "name_compare CI ordering");
    check(name_compare("ABC", "abc", NameCompare::CaseSensitive) != 0,
          "name_compare CS distinct");
    check(name_equals("Foo.NIF", "foo.nif", NameCompare::CaseInsensitive),
          "name_equals CI");
    check(!name_equals("foo", "Foo", NameCompare::CaseSensitive),
          "name_equals CS distinct");
    std::printf("PASS: name comparison\n");

    // --- 2. entry API over a directory tree --------------------------------
    {
        TempDir env;
        make_dir_tree(env.root, {
                                    {"Sub/b.bin", "bb"},
                                    {"Sub/empty.noext", ""},
                                    {"a.txt", "a"},
                                    {"noext", "x"},
                                });
        auto tree = FileTree::make_tree_from_directory(env.root);
        check(tree != nullptr, "dir tree created");
        check(tree->size() == 3, "dir tree has 3 root children");
        check(tree->at(0)->is_dir() || tree->at(1)->is_dir(),
              "root children sorted dirs-first");
        check(tree->exists("Sub"), "exists dir");
        check(tree->exists("a.txt"), "exists file");
        check(!tree->exists("missing"), "not exists missing");

        auto sub = tree->find_directory("sub");  // CI
        check(sub != nullptr, "find_directory CI");
        if (sub) {
            check(sub->name() == "Sub", "find_directory keeps real casing");
            auto b = sub->find("b.BIN");  // CI suffix
            check(b != nullptr && b->is_file(), "find file CI");
            if (b) {
                check(b->suffix() == "bin", "suffix lowercase");
                check(b->has_suffix("BIN"), "has_suffix CI");
                // path() walks to the root and includes the root's own name
                // (MO2 FileTreeEntry::path); path_to() gives the root-relative
                // path - the format find() consumes.
                check(b->path().size() >= 9 &&
                          b->path().substr(b->path().size() - 9) == "Sub/b.bin",
                      "entry path ends at root-relative path");
                check(tree->path_to(b) == "Sub/b.bin", "path_to from tree");
                check(b->parent() != nullptr, "entry parent");
            }
        }

        // '\\'-separated Windows-native path resolves too
        check(tree->find("Sub\\b.bin") != nullptr, "find backslash separator");

        // extension-less file
        auto noext = tree->find("noext");
        check(noext != nullptr && noext->suffix().empty(),
              "extension-less suffix empty");
        auto empty_ext = tree->find("Sub/empty.noext");
        check(empty_ext != nullptr && empty_ext->suffix() == "noext",
              "zero-length file still has suffix");

        // find_directory on a file returns null
        check(tree->find_directory("a.txt") == nullptr,
              "find_directory rejects files");

        // path_from with an ancestor tree
        auto top = sub->find("b.bin");
        if (top) check(top->path_from(sub) == "b.bin", "path_from subtree");

        // walk CONTINUE visits everything
        std::vector<std::string> visited;
        tree->walk(
            [&visited](const std::string& prefix,
                       const FileTree::const_reference& entry) {
                visited.push_back(prefix + entry->name());
                return FileTree::WalkReturn::Continue;
            });
        check(std::find(visited.begin(), visited.end(), "Sub/b.bin") != visited.end() &&
                  std::find(visited.begin(), visited.end(), "a.txt") != visited.end(),
              "walk visits all entries");
        std::printf("PASS: directory tree + entry API\n");
    }

    // --- 3. case-sensitive mode --------------------------------------------
    {
        TempDir env;
        make_dir_tree(env.root, {
                                    {"Meshes/x.nif", "x"},
                                    {"meshes/y.nif", "y"},
                                });
        auto cs = FileTree::make_tree_from_directory(env.root, NameCompare::CaseSensitive);
        check(cs != nullptr && cs->exists("Meshes") && cs->exists("meshes"),
              "CS mode keeps both casings");
        check(!cs->exists("MESHES"), "CS mode rejects wrong casing");
        auto ci = FileTree::make_tree_from_directory(env.root);
        check(ci != nullptr && ci->exists("MESHES"), "CI mode matches wrong casing");
        std::printf("PASS: case-sensitive tree\n");
    }

    // --- 4. walk STOP / SKIP ------------------------------------------------
    {
        TempDir env;
        make_dir_tree(env.root, {
                                    {"SkipMe/inner.txt", "i"},
                                    {"Keep/k.txt", "k"},
                                });
        auto tree = FileTree::make_tree_from_directory(env.root);
        int stopped = 0;
        tree->walk(
            [&stopped](const std::string&, const FileTree::const_reference&) {
                ++stopped;
                return FileTree::WalkReturn::Stop;
            });
        check(stopped == 1, "walk Stop halts after first entry");

        std::vector<std::string> kept;
        tree->walk(
            [&kept](const std::string& prefix,
                    const FileTree::const_reference& entry) {
                if (entry->is_dir() && entry->name() == "SkipMe") {
                    return FileTree::WalkReturn::Skip;
                }
                kept.push_back(prefix + entry->name());
                return FileTree::WalkReturn::Continue;
            });
        check(std::find(kept.begin(), kept.end(), "SkipMe/inner.txt") == kept.end(),
              "walk Skip prunes a directory subtree");
        check(std::find(kept.begin(), kept.end(), "Keep/k.txt") != kept.end(),
              "walk Skip keeps siblings");
        std::printf("PASS: walk Stop/Skip\n");
    }

    // --- 5. shape equivalence: archive tree == folder tree -------------------
    {
        TempDir env;
        std::vector<ZipItem> items = {
            {"meshes/foo.nif", "foo"},
            {"meshes/bar.nif", "bar"},
            {"textures/tex.dds", "t"},
            {"SKSE/Plugins/version.dll", "v"},
            {"docs/readme.txt", "r"},
            {"empty/", "", true},
            {"top.txt", "top"},
        };
        auto zip_path = env.root / "mod.zip";
        check(make_zip(zip_path, items), "zip written via libarchive");
        make_dir_tree(env.root / "folder", items);

        std::string err;
        auto atree = FileTree::make_tree_from_archive(zip_path, &err);
        check(atree != nullptr, "archive tree opened");
        auto ftree = FileTree::make_tree_from_directory(env.root / "folder");
        check(ftree != nullptr, "folder tree opened");
        if (atree && ftree) {
            assert_same_shape(atree, ftree,
                              "archive and folder trees have the same shape");
            check(atree->exists("empty"), "archive tree has empty dir");
            auto empty = atree->find_directory("empty");
            check(empty != nullptr && empty->empty(),
                  "empty dir populated as empty");
        }
        std::printf("PASS: archive/folder shape equivalence\n");
    }

    // --- 6. analyze_staging_layout on trees --------------------------------
    auto run_scenario =
        [](const char* what, const std::vector<ZipItem>& items,
           bool expect_fomod, bool expect_simple, bool expect_merged,
           const std::string& expect_hint,
           const std::vector<std::string>& expect_chain) {
            TempDir env;
            auto zip_path = env.root / "mod.zip";
            if (!make_zip(zip_path, items)) {
                check(false, "zip written");
                return;
            }
            make_dir_tree(env.root / "folder", items);

            std::string err;
            auto atree = FileTree::make_tree_from_archive(zip_path, &err);
            auto ftree = FileTree::make_tree_from_directory(env.root / "folder",
                                                            NameCompare::CaseInsensitive,
                                                            /*ignore_meta_ini=*/false);
            auto av = atree ? analyze_staging_layout(atree, "Data") : StagingNormalizeResult{};
            auto fv = ftree ? analyze_staging_layout(ftree, "Data") : StagingNormalizeResult{};

            check(same_verdict(av, fv), what);
            check(av.fomod == expect_fomod && av.simple == expect_simple &&
                      av.merged_data_dir == expect_merged &&
                      av.peeled_folder_hint == expect_hint &&
                      av.peel_chain == expect_chain,
                  what);
        };

    run_scenario("peel a non-data wrapper",
                 {{"Wrapper/SKSE/Plugins/x.dll", "x"}}, false, true, false, "Wrapper",
                 {"Wrapper"});
    run_scenario("merge a lone Data+readme top layer",
                 {{"Data/meshes/foo.nif", "foo"}, {"readme.txt", "r"}}, false, true,
                 true, "", {});
    run_scenario("never reshape FOMOD archives",
                 {{"fomod/ModuleConfig.xml", "<config/>"}, {"meshes/m.nif", "m"}}, true,
                 false, false, "", {});
    run_scenario("find a FOMOD inside a wrapper",
                 {{"ModName/fomod/ModuleConfig.xml", "<config/>"}}, true, false, false,
                 "", {});
    run_scenario("plain root (no data, multi-entry) is left as-is",
                 {{"readme.txt", "r"}, {"license.txt", "l"}}, false, false, false, "",
                 {});
    run_scenario("real data folder at root is never peeled",
                 {{"SKSE/Plugins/x.dll", "x"}, {"readme.txt", "r"}}, false, true, false,
                 "", {});
    run_scenario("deep wrapper chain peels every level",
                 {{"A/B/SKSE/Plugins/x.dll", "x"}}, false, true, false, "A",
                 {"A", "B"});

    // ModDataChecker direct check (MO2 GamebryoModDataChecker port)
    {
        TempDir env;
        make_dir_tree(env.root, {{"fonts/f.fnt", "f"}, {"Plugin.esp", "e"}});
        auto tree = FileTree::make_tree_from_directory(env.root);
        check(ModDataChecker::data_looks_valid(tree),
              "ModDataChecker accepts a data folder");
        TempDir plain;
        make_dir_tree(plain.root, {{"readme.txt", "r"}});
        auto p = FileTree::make_tree_from_directory(plain.root);
        check(!ModDataChecker::data_looks_valid(p),
              "ModDataChecker rejects junk");
        std::printf("PASS: staging layout analysis on trees\n");
    }

    // --- 7. normalize_staging_root physical driver --------------------------
    {
        TempDir env;
        auto root = env.root / "staging";
        make_dir_tree(root, {{"Wrapper/SKSE/Plugins/x.dll", "x"}});
        auto r = normalize_staging_root(root, "Data");
        check(r.simple && r.peeled_folder_hint == "Wrapper",
              "physical driver peels the wrapper");
        check(fs::exists(root / "SKSE" / "Plugins" / "x.dll") &&
                  !fs::exists(root / "Wrapper"),
              "physical peel moved children up");
    }
    {
        TempDir env;
        auto root = env.root / "staging";
        make_dir_tree(root, {{"Data/meshes/foo.nif", "foo"}, {"readme.txt", "r"}});
        auto r = normalize_staging_root(root, "Data");
        check(r.simple && r.merged_data_dir,
              "physical driver merges a Data+readme top layer");
        check(fs::exists(root / "meshes" / "foo.nif") && !fs::exists(root / "Data"),
              "physical merge moved Data children up");
    }
    {
        TempDir env;
        auto root = env.root / "staging";
        make_dir_tree(root, {{"fomod/ModuleConfig.xml", "<config/>"},
                             {"meshes/m.nif", "m"}});
        auto r = normalize_staging_root(root, "Data");
        check(r.fomod && fs::exists(root / "fomod" / "ModuleConfig.xml"),
              "physical driver leaves FOMOD untouched");
    }
}
