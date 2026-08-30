// Offscreen GUI regression for the Mod Info -> Categories tab.
//
// The tab must render the instance-scoped engine::Category::Factory registry —
// what the current game's plugin registered and what
// SettingsController::set_game_info() refreshes from <instance>/categories.dat
// on every instance/game switch — not the generic MO2 default list. Covers:
// hierarchy display (parent/child depths), assignability (checkable items,
// ancestor auto-check), persistence to the mod meta as MO2's
// "General/category" CSV, and rebuild after a game switch.
//
// Hermetic: offscreen platform, throwaway /tmp dirs, no network, no plugins
// (the registry is driven through Category::Factory::load() exactly like
// SettingsController does).
#include "engine/mod/meta/mod_meta.h"
#include "engine/pipeline/plugin_host/category_factory.h"
#include "ui/modinfo/categories_tab.h"

#include <QApplication>
#include <QComboBox>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace {

struct NodeInfo {
  int id = 0;
  QString text;
  Qt::CheckState checked = Qt::Unchecked;
  bool checkable = false;
  int depth = 0;
};

void collect_nodes(QTreeWidgetItem *node, int depth, QVector<NodeInfo> &out) {
  for (int i = 0; i < node->childCount(); ++i) {
    QTreeWidgetItem *child = node->child(i);
    out.push_back({child->data(0, Qt::UserRole).toInt(), child->text(0),
                   child->checkState(0),
                   (child->flags() & Qt::ItemIsUserCheckable) != 0, depth});
    collect_nodes(child, depth + 1, out);
  }
}

QVector<NodeInfo> nodes_of(const ui::CategoriesTab &tab) {
  QVector<NodeInfo> out;
  const auto *tree = tab.findChild<QTreeWidget *>();
  if (tree)
    collect_nodes(tree->invisibleRootItem(), 0, out);
  return out;
}

// Writes a categories.dat (ID|Name|ParentID rows) like MO2/the plugins do.
void write_dat(const std::filesystem::path &path,
               const std::vector<const char *> &rows) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto *row : rows)
    out << row << '\n';
}

// Mirrors SettingsController::set_game_info(): the new instance's
// categories.dat replaces the registry (missing file keeps the current set).
void switch_game(const std::filesystem::path &dat) {
  engine::Category::Factory::instance().load(dat);
}

ui::ModInfoData make_data(const std::string &id,
                          const std::filesystem::path &instance_root,
                          const std::filesystem::path &meta_dir) {
  ui::ModInfoData data;
  data.id = QString::fromStdString(id);
  data.name = QString::fromStdString(id);
  data.instance_root = QString::fromStdString(instance_root.string());
  data.load_meta = [meta_dir, id] {
    return engine::ModMeta::load(meta_dir, id);
  };
  data.save_meta = [meta_dir, id](const engine::ModMeta &m) {
    return m.save(meta_dir, id);
  };
  return data;
}

} // namespace

TEST_CASE("categories tab", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_categories_tab/config";
  std::filesystem::remove_all("/tmp/gmm_categories_tab");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path base = "/tmp/gmm_categories_tab";
  const std::filesystem::path isaac_dat =
      base / "instances/Isaac/categories.dat";
  const std::filesystem::path skyrim_dat =
      base / "instances/Skyrim/categories.dat";
  // Isaac-shaped registry (ids mirror the real plugin): Items with a child,
  // plus a root-level Lua category.
  write_dat(isaac_dat,
            {"1000|Items|0", "1001|Active Items|1000", "1006|Lua|0"});
  // A different game's set, including a generic MO2 name ("Animations").
  write_dat(skyrim_dat, {"1|Animations|0", "2|Armour|0", "52|Poses|1"});

  ui::CategoriesTab tab;

  SECTION("renders the current game's registry with hierarchy") {
    switch_game(isaac_dat);
    auto data = make_data("ModA", base / "instances/Isaac",
                          base / "instances/Isaac/meta");
    tab.set_current(data);
    tab.set_mod(data);

    const auto nodes = nodes_of(tab);
    REQUIRE(nodes.size() == 3);
    // Depth-first walk, siblings alphabetized: Items, its child Active
    // Items, then Lua.
    REQUIRE(nodes[0].id == 1000);
    REQUIRE(nodes[0].text == QStringLiteral("Items"));
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].id == 1001);
    REQUIRE(nodes[1].text == QStringLiteral("Active Items"));
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(nodes[2].id == 1006);
    REQUIRE(nodes[2].text == QStringLiteral("Lua"));
    REQUIRE(nodes[2].depth == 0);

    // Every node is user-checkable and starts unchecked; no generic MO2
    // defaults leaked in (the registry holds only the game's categories).
    for (const auto &n : nodes) {
      REQUIRE(n.checkable);
      REQUIRE(n.checked == Qt::Unchecked);
      REQUIRE(n.id != 1); // "Animations" & co. must stay absent
    }

    // Metadata has no category yet: the primary combo starts empty.
    auto *combo = tab.findChild<QComboBox *>();
    REQUIRE(combo != nullptr);
    REQUIRE(combo->count() == 0);
  }

  SECTION("assignment persists ids with ancestors auto-checked") {
    switch_game(isaac_dat);
    auto data = make_data("ModA", base / "instances/Isaac",
                          base / "instances/Isaac/meta");
    tab.set_current(data);
    tab.set_mod(data);

    auto *tree = tab.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    // topLevelItem(0) = Items; its first child = Active Items.
    QTreeWidgetItem *active_items = tree->topLevelItem(0)->child(0);
    REQUIRE(active_items != nullptr);
    REQUIRE(active_items->data(0, Qt::UserRole).toInt() == 1001);

    active_items->setCheckState(0, Qt::Checked);

    // Checking the child auto-checks its ancestor (MO2 behavior).
    const auto nodes = nodes_of(tab);
    REQUIRE(nodes[0].id == 1000);
    REQUIRE(nodes[0].checked == Qt::Checked);
    REQUIRE(nodes[1].checked == Qt::Checked);
    REQUIRE(nodes[2].checked == Qt::Unchecked);

    // Persisted as MO2's "category" CSV of internal ids (primary first;
    // no explicit primary here, so tree order).
    const auto meta =
        engine::ModMeta::load(base / "instances/Isaac/meta", "ModA");
    REQUIRE(meta.get("General", "category") == "1000,1001");

    // Reloading the mod restores the checked state from the metadata.
    tab.set_mod(data);
    const auto reloaded = nodes_of(tab);
    REQUIRE(reloaded[0].checked == Qt::Checked);
    REQUIRE(reloaded[1].checked == Qt::Checked);
    REQUIRE(reloaded[2].checked == Qt::Unchecked);
  }

  SECTION("rebuilds after an instance/game switch") {
    switch_game(isaac_dat);
    auto data_a = make_data("ModA", base / "instances/Isaac",
                            base / "instances/Isaac/meta");
    tab.set_current(data_a);
    tab.set_mod(data_a);
    REQUIRE(nodes_of(tab).size() == 3);
    REQUIRE(nodes_of(tab)[0].text == QStringLiteral("Items"));

    // Switch games: the new instance's categories.dat replaces the
    // registry, and the next set_mod() must consume it.
    switch_game(skyrim_dat);
    auto data_b = make_data("ModB", base / "instances/Skyrim",
                            base / "instances/Skyrim/meta");
    tab.set_current(data_b);
    tab.set_mod(data_b);

    const auto nodes = nodes_of(tab);
    REQUIRE(nodes.size() == 3);
    // Old game's categories are gone.
    for (const auto &n : nodes) {
      REQUIRE(n.text != QStringLiteral("Items"));
      REQUIRE(n.text != QStringLiteral("Lua"));
    }
    // New hierarchy: Animations (root), Poses (its child), Armour (root).
    REQUIRE(nodes[0].id == 1);
    REQUIRE(nodes[0].text == QStringLiteral("Animations"));
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].id == 52);
    REQUIRE(nodes[1].text == QStringLiteral("Poses"));
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(nodes[2].id == 2);
    REQUIRE(nodes[2].text == QStringLiteral("Armour"));
    REQUIRE(nodes[2].depth == 0);
  }
}
