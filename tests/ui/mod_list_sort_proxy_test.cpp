#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_list_sort_proxy.h"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QMimeData>
#include <QHash>

#include <vector>

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

TEST_CASE("ModListSortProxy: per-column comparator", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_separator("sep", "Sep", "");
  m.add_mod("z", "zebra", "1.0");
  m.add_mod("a", "apple", "1.0");
  m.add_mod("e", "", "1.0");
  m.add_mod("native", "native", "1.0", -1, true);

  // helper to find source row by name
  auto src_row_of = [&m](const QString &name) {
    for (int i = 0; i < m.mods().size(); ++i)
      if (m.mods()[i].name == name)
        return i;
    return -1;
  };
  const int rZebra = src_row_of("zebra");
  const int rApple = src_row_of("apple");
  const int rEmpty = src_row_of("");
  const int rNative = src_row_of("native");

  ui::ModListSortProxy proxy(&m);

  // Priority ascending is identity
  proxy.sort(ui::ModList::Priority, Qt::AscendingOrder);
  {
    for (int i = 0; i < m.mods().size(); ++i)
      REQUIRE(proxy.mapToSource(proxy.index(i, 0)).row() == i);
  }

  // Name ascending: case-insensitive, empty last
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);
  {
    auto pos_of = [&](int src) {
      for (int i = 0; i < m.mods().size(); ++i)
        if (proxy.mapToSource(proxy.index(i, 0)).row() == src)
          return i;
      return -1;
    };
    REQUIRE(pos_of(rApple) < pos_of(rZebra));
    REQUIRE(pos_of(rZebra) < pos_of(rEmpty));
  }

  // Name descending reverses the non-fixed rows
  proxy.sort(ui::ModList::Name, Qt::DescendingOrder);
  {
    auto pos_of = [&](int src) {
      for (int i = 0; i < m.mods().size(); ++i)
        if (proxy.mapToSource(proxy.index(i, 0)).row() == src)
          return i;
      return -1;
    };
    REQUIRE(pos_of(rEmpty) < pos_of(rZebra));
    REQUIRE(pos_of(rZebra) < pos_of(rApple));
  }
}

TEST_CASE("ModListSortProxy: dropMimeData guard", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_mod("a", "Alpha", "1.0");
  m.add_mod("b", "Beta", "1.0");
  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);
  QMimeData data;
  data.setData(QString::fromLatin1(ui::kModListMimeType), QByteArray());
  // refused: returns false
  CHECK_FALSE(proxy.dropMimeData(&data, Qt::MoveAction, 0, 0, QModelIndex()));
  proxy.sort(ui::ModList::Priority, Qt::AscendingOrder);
  // allowed path: base may accept or not, but not refused by guard; just ensure no crash
  (void)proxy.dropMimeData(&data, Qt::MoveAction, 0, 0, QModelIndex());
}

TEST_CASE("ModListSortProxy: fold + sort keeps members hidden", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_separator("sep", "Sep", "");
  m.add_mod("a", "apple", "1.0", 100);
  m.add_mod("b", "zebra", "1.0", 101);
  // find separator row
  int sep_row = -1;
  for (int i = 0; i < m.mods().size(); ++i)
    if (m.mods()[i].is_separator) { sep_row = i; break; }
  m.set_folded(sep_row, true);
  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);
  // the two mods are fold-hidden
  for (int i = 0; i < m.mods().size(); ++i) {
    if (m.mods()[i].is_separator || m.mods()[i].is_overwrite)
      continue;
    CHECK(m.is_row_fold_hidden(i));
  }
}

TEST_CASE("ModListSortProxy: filter + sort composition", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_mod("a", "apple", "1.0", 100);
  m.add_mod("b", "zebra", "1.0", 101);
  // publish a view_visible that hides zebra
  QVector<bool> vis(m.mods().size(), true);
  for (int i = 0; i < m.mods().size(); ++i)
    if (m.mods()[i].name == "zebra") vis[i] = false;
  m.set_view_visible(vis);
  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);
  // zebra hidden, apple visible
  for (int i = 0; i < m.mods().size(); ++i) {
    if (m.mods()[i].name == "zebra")
      CHECK_FALSE(proxy.filterAcceptsRow(i, QModelIndex()));
    if (m.mods()[i].name == "apple")
      CHECK(proxy.filterAcceptsRow(i, QModelIndex()));
  }
}

TEST_CASE("ModListSortProxy: separator block moves as a unit", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  // Source order is NOT name order: Core, Utils, Early -> sorted Core, Early, Utils
  m.add_separator("sepA", "Core", "");
  m.add_mod("a1", "childA1", "1.0", 100);
  m.add_mod("a2", "childA2", "1.0", 101);
  m.add_separator("sepB", "Utils", "");
  m.add_mod("b1", "childB1", "1.0", 102);
  m.add_separator("sepC", "Early", "");
  m.add_mod("c1", "childC1", "1.0", 103);

  auto src_row_of = [&m](const QString &name) {
    for (int i = 0; i < m.mods().size(); ++i)
      if (m.mods()[i].name == name)
        return i;
    return -1;
  };
  const int rSepA = src_row_of("Core");
  const int rSepB = src_row_of("Utils");
  const int rSepC = src_row_of("Early");
  const int rA1   = src_row_of("childA1");
  const int rA2   = src_row_of("childA2");
  const int rB1   = src_row_of("childB1");
  const int rC1   = src_row_of("childC1");

  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);

  auto pos_of = [&](int src) {
    for (int i = 0; i < m.mods().size(); ++i)
      if (proxy.mapToSource(proxy.index(i, 0)).row() == src)
        return i;
    return -1;
  };

  // Separators sorted by name: Core < Early < Utils
  REQUIRE(pos_of(rSepA) < pos_of(rSepC));
  REQUIRE(pos_of(rSepC) < pos_of(rSepB));
  // Each block stays contiguous
  REQUIRE(pos_of(rA1) == pos_of(rSepA) + 1);
  REQUIRE(pos_of(rA2) == pos_of(rSepA) + 2);
  REQUIRE(pos_of(rC1) == pos_of(rSepC) + 1);
  REQUIRE(pos_of(rB1) == pos_of(rSepB) + 1);
}

TEST_CASE("ModListSortProxy: nested separator block", "[ui][sort]") {
  AppGuard g;
  ui::ModList m;
  m.add_separator("outer", "Outer", "");
  m.add_separator("inner", "Inner", "");
  m.add_mod("x", "childX", "1.0", 100);
  m.add_separator("other", "Other", "");

  // nest inner under outer
  QHash<QString, QString> links;
  links.insert("inner", "outer");
  m.restore_parent_links(links);
  m.set_nesting_enabled(true);

  auto src_row_of = [&m](const QString &name) {
    for (int i = 0; i < m.mods().size(); ++i)
      if (m.mods()[i].name == name)
        return i;
    return -1;
  };
  const int rOuter = src_row_of("Outer");
  const int rInner = src_row_of("Inner");
  const int rX     = src_row_of("childX");
  const int rOther = src_row_of("Other");

  ui::ModListSortProxy proxy(&m);
  proxy.sort(ui::ModList::Name, Qt::AscendingOrder);

  auto pos_of = [&](int src) {
    for (int i = 0; i < m.mods().size(); ++i)
      if (proxy.mapToSource(proxy.index(i, 0)).row() == src)
        return i;
    return -1;
  };

  // Whole outer block (outer, inner, childX) moves together; inner follows outer
  REQUIRE(pos_of(rInner) == pos_of(rOuter) + 1);
  REQUIRE(pos_of(rX) == pos_of(rOuter) + 2);
}
