// Engine test for the MO2-compatible per-instance category database.
//
// Covers: MO2 default seed (ids/parents/children), round-tripping the real
// 3-cell categories.dat format (id|name|parentId), the 4-cell nexus variant,
// nexuscatmap.dat mapping + category_for_nexus, add/remove/set_parent, and
// rebuilding the tree (has_children, dangling-parent pruning).
#include "engine/meta/categories.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void require(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

int main() {
    using engine::Categories;

    // --- Default seed (MO2's loadDefaultCategories). ---
    Categories seeded;
    require(seeded.contains(1) && seeded.contains(47), "seed contains MO2 ids");
    require(!seeded.contains(0), "id 0 (None) is implicit, not in list");
    const auto* animations = seeded.find(1);
    require(animations && animations->name == "Animations",
            "category 1 is Animations");
    require(animations->parent_id == 0, "Animations is a top-level category");
    const auto* poses = seeded.find(52);
    require(poses && poses->parent_id == 1, "Poses is a child of Animations");
    auto children = seeded.children_of(1);
    require(children.size() == 1 && children[0]->id == 52,
            "Animations has exactly one child: Poses");
    const auto* tats = seeded.find(39);
    require(tats && tats->name == "Tattoos",
            "duplicate id 39 resolves to the last entry (Tattoos)");

    // --- Round-trip the real MO2 categories.dat (3-cell form). ---
    const fs::path root = "/tmp/gmm_categories_test/instances/Skyrim";
    fs::remove_all(root.parent_path());
    fs::create_directories(root);
    write_file(root / "categories.dat",
               "1|Animations|0\n"
               "52|Poses|1\n"
               "2|Armour|0\n"
               "39|Voice|0\n"
               "10|Body, Face, & Hair|0\n"
               "39|Tattoos|10\n");
    write_file(root / "nexuscatmap.dat",
               "1|Miscellaneous|13\n"
               "1|Skins Textures|25\n");

    Categories loaded = Categories::load(root);
    require(loaded.contains(1) && loaded.contains(52),
            "load reads categories.dat");
    require(loaded.children_of(1).size() == 1 && loaded.children_of(1)[0]->id == 52,
            "Poses is child of Animations after load");
    require(loaded.children_of(10).size() == 1 &&
                loaded.children_of(10)[0]->id == 39,
            "Tattoos (39) is child of Body/Face/Hair (10)");
    const auto* voice = loaded.find(39);
    require(voice && voice->name == "Tattoos",
            "duplicate id 39 lookup resolves last-wins (MO2 ID map)");
    int voice_entries = 0;
    for (const auto& c : loaded.categories())
        if (c.id == 39 && c.name == "Voice") ++voice_entries;
    require(voice_entries == 1, "both id-39 entries stay in the vector");
    require(loaded.contains(13) == false, "nexus ids are not internal categories");

    // --- Nexus mapping. ---
    require(loaded.has_nexus(13), "nexuscatmap.dat loaded");
    const auto* nc = loaded.nexus(13);
    require(nc && nc->name == "Miscellaneous" && nc->category_id == 1,
            "nexus 13 -> internal 1 (Miscellaneous)");
    const auto* cat = loaded.category_for_nexus(25);
    require(cat && cat->name == "Animations", "nexus 25 maps to Animations");

    // --- Save reproduces the source files. ---
    loaded.save(root);
    std::ifstream cat_in(root / "categories.dat");
    std::string saved((std::istreambuf_iterator<char>(cat_in)),
                      std::istreambuf_iterator<char>());
    require(saved == "1|Animations|0\n52|Poses|1\n2|Armour|0\n"
                     "39|Voice|0\n10|Body, Face, & Hair|0\n39|Tattoos|10\n",
            "categories.dat round-trips byte-identically");
    std::ifstream map_in(root / "nexuscatmap.dat");
    std::string saved_map((std::istreambuf_iterator<char>(map_in)),
                          std::istreambuf_iterator<char>());
    require(saved_map == "1|Miscellaneous|13\n1|Skins Textures|25\n",
            "nexuscatmap.dat round-trips byte-identically");

    // --- 4-cell nexus variant. ---
    write_file(root / "categories.dat",
               "1|Animations|13,25|0\n2|Armour|0\n");
    write_file(root / "nexuscatmap.dat", "");
    Categories four = Categories::load(root);
    const auto* anim = four.find(1);
    require(anim && anim->nexus_ids.size() == 2 && anim->nexus_ids[0] == 13,
            "4-cell form parses nexus ids");

    // --- Mutation + tree rebuild. ---
    Categories cats;
    cats.add_category(100, "My Mods", 0);
    cats.add_category(101, "Child Mods", 100);
    require(cats.find(100)->has_children,
            "has_children true once a child is added");
    cats.set_parent(101, 999);  // dangling parent
    cats.rebuild_tree();
    require(!cats.contains(101), "dangling-parent category pruned");
    require(cats.contains(100), "valid category survives rebuild");
    const auto* mymods = cats.find(100);
    require(mymods && mymods->has_children == false,
            "has_children false after child pruned");
    cats.add_category(101, "Child Mods", 100);
    cats.rebuild_tree();
    require(cats.children_of(100).size() == 1 && cats.children_of(100)[0]->id == 101,
            "re-added child shows under its parent");
    require(cats.find(100)->has_children, "has_children recomputed");

    // --- add_nexus_mapping populates the catmap (single source of truth). ---
    Categories m;
    m.add_category(1, "Gameplay", 0);
    m.add_nexus_mapping(1, "Gameplay Mechanics", 52);
    require(m.nexus(52) && m.nexus(52)->category_id == 1 &&
                m.nexus(52)->name == "Gameplay Mechanics",
            "nexus mapping added");
    require(m.category_for_nexus(52) && m.category_for_nexus(52)->name == "Gameplay",
            "category_for_nexus resolves through the map");

    std::printf("categories_test: all checks passed\n");
    return 0;
}
