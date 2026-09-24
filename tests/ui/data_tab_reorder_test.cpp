// DataTab - incremental conflict update on mod reorder (Workspace-z1iu).
//
// Moving a mod in the priority list used to clear the whole data tree and
// recreate all rows. The tab now diffs the freshly built row set against the
// applied one and updates changed rows in place. This pins that behavior:
// after a winner swap the tree items keep their identity (no recreate) and
// show the new winner, and adding a file inserts one row without touching
// the rest.
//
// Hermetic: offscreen platform, throwaway /tmp/opencode tree, tiny registry
// (the background build + chunked apply are pumped on the main thread).
#include "ui/panels/data_tab.h"
#include "ui/widgets/mod_list_model.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Registry =
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;

void write_file(const std::filesystem::path &path, const char *content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

// Pump the main thread until cond() holds or the deadline expires. The data
// tab builds rows on its worker thread and chunks them into the tree via
// queued signals / singleShot timers, so the test must spin the event loop.
template <typename Pred>
bool wait_for(Pred cond, int timeout_ms = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(5);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  return true;
}

QTreeWidgetItem *find_top_row(QTreeWidget *tree, const QString &name) {
  auto *root = tree->invisibleRootItem();
  for (int i = 0; i < root->childCount(); ++i) {
    if (root->child(i)->text(0) == name)
      return root->child(i);
  }
  return nullptr;
}

}  // namespace

TEST_CASE("data tab updates winners in place on reorder", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  const std::filesystem::path base = "/tmp/opencode/gmm_data_tab_reorder";
  std::filesystem::remove_all(base);
  const std::filesystem::path mods = base / "mods";
  write_file(mods / "ModA" / "shared.txt", "aaa\n");
  write_file(mods / "ModA" / "a_only.txt", "a\n");
  write_file(mods / "ModB" / "shared.txt", "bbbb\n");
  write_file(mods / "ModB" / "b_only.txt", "b\n");
  write_file(mods / "ModC" / "c_only.txt", "c\n");

  ui::ModEntry mod_a;
  mod_a.id       = "ModA";
  mod_a.name     = "Mod A";
  mod_a.priority = 2;
  ui::ModEntry mod_b;
  mod_b.id       = "ModB";
  mod_b.name     = "Mod B";
  mod_b.priority = 1;
  ui::ModEntry mod_c;
  mod_c.id       = "ModC";
  mod_c.name     = "Mod C";
  mod_c.priority = 3;
  const QVector<ui::ModEntry> all_mods{mod_a, mod_b, mod_c};

  ui::DataTab tab;
  auto *tree = tab.tree();
  REQUIRE(tree);

  const Registry v1{{"shared.txt", {{"ModA", 2}, {"ModB", 1}}},
                    {"a_only.txt", {{"ModA", 2}}},
                    {"b_only.txt", {{"ModB", 1}}}};
  tab.show_data(v1, all_mods, false, mods, {}, {}, "", "Data", false);
  REQUIRE(wait_for([&] {
    return find_top_row(tree, "shared.txt") != nullptr;
  }));

  auto *shared = find_top_row(tree, "shared.txt");
  auto *a_only = find_top_row(tree, "a_only.txt");
  auto *b_only = find_top_row(tree, "b_only.txt");
  REQUIRE(shared);
  REQUIRE(a_only);
  REQUIRE(b_only);
  CHECK(shared->text(2) == "Mod A");
  CHECK(shared->text(3) == "2");
  CHECK(a_only->text(3).isEmpty());

  // Reorder: ModB now outranks ModA. Same path set, only the winner flips.
  const Registry v2{{"shared.txt", {{"ModA", 1}, {"ModB", 2}}},
                    {"a_only.txt", {{"ModA", 1}}},
                    {"b_only.txt", {{"ModB", 2}}}};
  tab.show_data(v2, all_mods, false, mods, {}, {}, "", "Data", false);
  REQUIRE(wait_for([&] {
    auto *s = find_top_row(tree, "shared.txt");
    return s != nullptr && s->text(2) == "Mod B";
  }));

  // Incremental: no item was recreated, the winner cell updated in place.
  CHECK(find_top_row(tree, "shared.txt") == shared);
  CHECK(find_top_row(tree, "a_only.txt") == a_only);
  CHECK(find_top_row(tree, "b_only.txt") == b_only);
  CHECK(shared->text(3) == "2");

  // Adding a file inserts one row and leaves the existing items alone.
  Registry v3 = v2;
  v3.emplace("c_only.txt", std::vector<std::pair<std::string, int>>{{"ModC", 3}});
  tab.show_data(v3, all_mods, false, mods, {}, {}, "", "Data", false);
  REQUIRE(wait_for([&] {
    return find_top_row(tree, "c_only.txt") != nullptr;
  }));
  CHECK(find_top_row(tree, "shared.txt") == shared);
  CHECK(find_top_row(tree, "a_only.txt") == a_only);
  CHECK(find_top_row(tree, "b_only.txt") == b_only);
  CHECK(find_top_row(tree, "c_only.txt")->text(2) == "Mod C");

  std::filesystem::remove_all(base);
}
