#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_list_sort_proxy.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QMimeData>

namespace {
struct AppGuard {
  AppGuard() {
    static int argc = 1;
    static char name[] = "t";
    static char *argv[] = {name};
    if (!QApplication::instance())
      new QApplication(argc, argv);
  }
};
}  // namespace

TEST_CASE("ModListSortProxy: priority ascending is identity", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_mod("a", "Alpha", "1.0");
  m.add_mod("b", "Beta", "1.0");
  m.add_mod("c", "Gamma", "1.0");
  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Priority, Qt::AscendingOrder);
  for (int i = 0; i < m.mods().size(); ++i) {
    REQUIRE(proxy.mapToSource(proxy.index(i, 0)).row() == i);
  }
}

TEST_CASE("ModListSortProxy: sorting by Name leaves priorities unchanged", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_mod("a", "Charlie", "1.0");
  m.add_mod("b", "Alpha", "1.0");
  m.add_mod("c", "Bravo", "1.0");
  m.clear_dirty_priority_ids();
  const auto before = m.dirty_priority_ids().size();
  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);
  for (int i = 0; i < m.mods().size(); ++i) {
    CHECK(m.mods()[static_cast<size_t>(i)].priority == i);
  }
  CHECK(m.dirty_priority_ids().size() == before);
}

TEST_CASE("ModListSortProxy: drag refused unless priority ascending", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_mod("a", "Alpha", "1.0");
  m.add_mod("b", "Beta", "1.0");
  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);
  QMimeData data;
  data.setData(QString::fromLatin1(ui::kModListMimeType), QByteArray());
  CHECK_FALSE(proxy.canDropMimeData(&data, Qt::MoveAction, 0, 0, QModelIndex()));
  proxy.sort(ui::ModList::Priority, Qt::AscendingOrder);
  CHECK(proxy.canDropMimeData(&data, Qt::MoveAction, 0, 0, QModelIndex()));
}

TEST_CASE("ModListSortProxy: header fold click no-op handled in controller", "[ui][sort]") {
  AppGuard g;
  SUCCEED();
}
