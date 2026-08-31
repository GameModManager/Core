// Offscreen GUI test for the MO2-style category filter panel.
//
// Covers: tree built from engine::Category::Factory (parent/child hierarchy,
// alphabetized siblings), checkable items, checked_category_ids /
// has_active_filter, Clear unchecking everything, and the
// category_filter_changed signal on toggle and Clear.
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME, no network.
#include "engine/pipeline/plugin_host/category_factory.h"
#include "ui/widgets/category_filter_panel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {

// Recursive walk: collect the (id, text, checked) triples in tree order.
struct NodeInfo {
  int id = 0;
  QString text;
  Qt::CheckState checked = Qt::Unchecked;
  int depth = 0;
};

void collect_nodes(QTreeWidgetItem *node, int depth, QVector<NodeInfo> &out) {
  for (int i = 0; i < node->childCount(); ++i) {
    QTreeWidgetItem *child = node->child(i);
    out.push_back({child->data(0, Qt::UserRole).toInt(), child->text(0),
                   child->checkState(0), depth});
    collect_nodes(child, depth + 1, out);
  }
}

QVector<NodeInfo> nodes_of(const ui::CategoryFilterPanel &panel) {
  // Reach the tree through the widget hierarchy: the panel owns a
  // QTreeWidget as its first child.
  QVector<NodeInfo> out;
  const auto *tree = panel.findChild<QTreeWidget *>();
  if (tree)
    collect_nodes(tree->invisibleRootItem(), 0, out);
  return out;
}

} // namespace

TEST_CASE("category filter panel", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/gmm_category_filter_panel/config";
  std::filesystem::remove_all("/tmp/gmm_category_filter_panel");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  // Seed the global factory (the test binary starts with an empty registry).
  auto &factory = engine::Category::Factory::instance();
  factory.removeCategory(1);
  factory.removeCategory(2);
  factory.removeCategory(3);
  factory.removeCategory(4);
  factory.addCategory(1, "Animations", 0);
  factory.addCategory(2, "Armour", 0);
  factory.addCategory(3, "Poses", 1); // child of Animations
  factory.addCategory(4, "Idles", 1); // child of Animations

  ui::CategoryFilterPanel panel;
  QSignalSpy changed_spy(&panel,
                         &ui::CategoryFilterPanel::category_filter_changed);

  SECTION("tree mirrors the factory hierarchy") {
    const auto nodes = nodes_of(panel);
    REQUIRE(nodes.size() == 4);
    // Depth-first walk: Animations (root), then its children sorted by
    // name (Idles before Poses), then Armour (root).
    REQUIRE(nodes[0].id == 1);
    REQUIRE(nodes[0].text == "Animations");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].id == 4);
    REQUIRE(nodes[1].text == "Idles");
    REQUIRE(nodes[1].depth == 1);
    REQUIRE(nodes[2].id == 3);
    REQUIRE(nodes[2].text == "Poses");
    REQUIRE(nodes[2].depth == 1);
    REQUIRE(nodes[3].id == 2);
    REQUIRE(nodes[3].text == "Armour");
    REQUIRE(nodes[3].depth == 0);
    // Every item is user-checkable and starts unchecked.
    for (const auto &n : nodes) {
      REQUIRE(n.checked == Qt::Unchecked);
    }
    REQUIRE_FALSE(panel.has_active_filter());
    REQUIRE(panel.checked_category_ids().isEmpty());
  }

  SECTION("checking a category is reported and emits the signal") {
    auto *tree = panel.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    auto *anim = tree->topLevelItem(0);
    REQUIRE(anim != nullptr);
    REQUIRE(anim->data(0, Qt::UserRole).toInt() == 1);

    anim->setCheckState(0, Qt::Checked);
    REQUIRE(changed_spy.count() == 1);
    REQUIRE(panel.has_active_filter());
    const auto checked = panel.checked_category_ids();
    REQUIRE(checked.size() == 1);
    REQUIRE(checked.contains(1));

    // Checking a child adds it independently (no parent cascade).
    auto *idles = anim->child(0);
    REQUIRE(idles != nullptr);
    idles->setCheckState(0, Qt::Checked);
    REQUIRE(changed_spy.count() == 2);
    const auto checked2 = panel.checked_category_ids();
    REQUIRE(checked2.size() == 2);
    REQUIRE(checked2.contains(1));
    REQUIRE(checked2.contains(4));
  }

  SECTION("clear unchecks everything and emits the signal") {
    auto *tree = panel.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    tree->topLevelItem(0)->setCheckState(0, Qt::Checked);
    tree->topLevelItem(1)->setCheckState(0, Qt::Checked);
    REQUIRE(panel.has_active_filter());

    panel.clear_filter();
    REQUIRE(changed_spy.count() == 3); // 2 toggles + 1 clear
    REQUIRE_FALSE(panel.has_active_filter());
    REQUIRE(panel.checked_category_ids().isEmpty());
    for (const auto &n : nodes_of(panel))
      REQUIRE(n.checked == Qt::Unchecked);
  }

  SECTION("rebuild keeps the tree but resets the checked state") {
    auto *tree = panel.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    tree->topLevelItem(0)->setCheckState(0, Qt::Checked);
    REQUIRE(panel.has_active_filter());

    panel.rebuild();
    REQUIRE_FALSE(panel.has_active_filter());
    REQUIRE(nodes_of(panel).size() == 4);
  }
}