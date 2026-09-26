// Double-click opens previews (Workspace-co2 + Workspace-636).
//
// co2 adds Settings::double_clicks_open_previews (default OFF) with a General
// tab checkbox; 636 consumes it in DataTab::on_item_double_clicked with
// Ctrl-swap, preview fallback to OS-open, and Enter-key activation.
//
//   - the setting defaults to false and round-trips through QSettings
//   - DataTab::resolve_double_click pins the setting x modifier x
//     file-type matrix (executables always execute - never preview an exe;
//     Alt always reveals the containing folder)
//   - reveal_dir pins the Alt target to the file's containing folder
//   - plain double-click integration: setting OFF opens, setting ON
//     previews when supported and opens otherwise
//
// Hermetic: offscreen platform, throwaway XDG_CONFIG_HOME + /tmp/opencode
// tree, tiny registry (the background build + chunked apply are pumped on
// the main thread like data_tab_reorder_test).
#include "ui/panels/data_tab.h"
#include "ui/settings/settings.h"
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

// Protected double-click internals, driven directly (keyboard modifiers
// cannot be faked offscreen, so the Ctrl half of the matrix is pinned via
// resolve_double_click below instead).
struct TestableDataTab : ui::DataTab {
  using ui::DataTab::on_item_double_clicked;
};

}  // namespace

TEST_CASE("settings double_clicks_open_previews persistence", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/opencode/gmm_dblclick_previews/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_dblclick_previews");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  auto &s = Settings::instance();

  CHECK(!s.double_clicks_open_previews());

  s.set_double_clicks_open_previews(true);
  CHECK(s.double_clicks_open_previews());

  s.set_double_clicks_open_previews(false);
  CHECK(!s.double_clicks_open_previews());

  std::filesystem::remove_all("/tmp/opencode/gmm_dblclick_previews");
}

TEST_CASE("data tab double-click action matrix", "[ui]") {
  using Action = ui::DataTab::DoubleClickAction;
  // Executables always execute - never preview an exe, whatever the
  // setting, modifier, or preview support says.
  CHECK(ui::DataTab::resolve_double_click(false, false, false, false, true) ==
        Action::Execute);
  CHECK(ui::DataTab::resolve_double_click(true, false, false, true, true) ==
        Action::Execute);
  CHECK(ui::DataTab::resolve_double_click(true, true, false, true, true) ==
        Action::Execute);
  // Setting OFF (default): plain opens, Ctrl previews.
  CHECK(ui::DataTab::resolve_double_click(false, false, false, true, false) ==
        Action::Open);
  CHECK(ui::DataTab::resolve_double_click(false, true, false, true, false) ==
        Action::Preview);
  // Setting ON: plain previews, Ctrl opens.
  CHECK(ui::DataTab::resolve_double_click(true, false, false, true, false) ==
        Action::Preview);
  CHECK(ui::DataTab::resolve_double_click(true, true, false, true, false) ==
        Action::Open);
  // No preview handler: fall back to OS-open in every mode.
  CHECK(ui::DataTab::resolve_double_click(false, false, false, false, false) ==
        Action::Open);
  CHECK(ui::DataTab::resolve_double_click(false, true, false, false, false) ==
        Action::Open);
  CHECK(ui::DataTab::resolve_double_click(true, false, false, false, false) ==
        Action::Open);
  CHECK(ui::DataTab::resolve_double_click(true, true, false, false, false) ==
        Action::Open);
  // Alt always reveals the containing folder, beating every other rule
  // (the setting, Ctrl, preview support and executables alike).
  CHECK(ui::DataTab::resolve_double_click(false, false, true, true, false) ==
        Action::Reveal);
  CHECK(ui::DataTab::resolve_double_click(false, true, true, true, false) ==
        Action::Reveal);
  CHECK(ui::DataTab::resolve_double_click(true, false, true, true, false) ==
        Action::Reveal);
  CHECK(ui::DataTab::resolve_double_click(true, true, true, true, false) ==
        Action::Reveal);
  CHECK(ui::DataTab::resolve_double_click(false, false, true, false, true) ==
        Action::Reveal);
}

TEST_CASE("alt double-click reveals the containing folder", "[ui]") {
  // Alt+double-click resolves to Reveal (above), and Reveal targets the
  // file's CONTAINING FOLDER in the OS file manager, not the file itself.
  // Only file rows carry a real path (directories have no DataRealPathRole),
  // so the containing folder is always the parent of the file.
  CHECK(ui::DataTab::resolve_double_click(false, false, true, true, false) ==
        ui::DataTab::DoubleClickAction::Reveal);
  CHECK(ui::DataTab::reveal_dir("/tmp/opencode/mods/ModA/textures/face.dds") ==
        "/tmp/opencode/mods/ModA/textures");
  CHECK(ui::DataTab::reveal_dir("/tmp/opencode/mods/ModA/readme.txt") ==
        "/tmp/opencode/mods/ModA");
  CHECK(ui::DataTab::reveal_dir("").isEmpty());
}

TEST_CASE("data tab double-click honors the previews setting", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path cfg = "/tmp/opencode/gmm_dblclick_datatab/config";
  std::filesystem::remove_all("/tmp/opencode/gmm_dblclick_datatab");
  std::filesystem::create_directories(cfg);
  qputenv("XDG_CONFIG_HOME", cfg.c_str());
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");

  const std::filesystem::path base = "/tmp/opencode/gmm_dblclick_datatab/tree";
  const std::filesystem::path mods = base / "mods";
  write_file(mods / "ModA" / "readme.txt", "hello\n");
  write_file(mods / "ModA" / "lib.dll", "MZ");
  write_file(mods / "ModA" / "tool.exe", "MZ");

  ui::ModEntry mod_a;
  mod_a.id       = "ModA";
  mod_a.name     = "Mod A";
  mod_a.priority = 1;
  const QVector<ui::ModEntry> all_mods{mod_a};

  TestableDataTab tab;
  auto *tree = tab.tree();
  REQUIRE(tree);

  int opens = 0, previews = 0, executes = 0;
  QObject::connect(&tab, &ui::DataTab::open_requested, &tab, [&](const QString &) {
    ++opens;
  });
  QObject::connect(&tab, &ui::DataTab::preview_requested, &tab,
                   [&](const QString &, const QStringList &, const QStringList &) {
                     ++previews;
                   });
  QObject::connect(&tab, &ui::DataTab::execute_requested, &tab,
                   [&](const QString &, bool, const QString &) {
                     ++executes;
                   });

  const Registry v1{{"readme.txt", {{"ModA", 1}}},
                    {"lib.dll", {{"ModA", 1}}},
                    {"tool.exe", {{"ModA", 1}}}};
  tab.show_data(v1, all_mods, false, mods, {}, {}, "", "Data", false);
  REQUIRE(wait_for([&] {
    return find_top_row(tree, "readme.txt") != nullptr &&
           find_top_row(tree, "lib.dll") != nullptr &&
           find_top_row(tree, "tool.exe") != nullptr;
  }));

  auto &s = Settings::instance();

  // Setting OFF (default): plain double-click OS-opens everything except
  // executables, which execute.
  s.set_double_clicks_open_previews(false);
  tab.on_item_double_clicked(find_top_row(tree, "readme.txt"), 0);
  tab.on_item_double_clicked(find_top_row(tree, "lib.dll"), 0);
  tab.on_item_double_clicked(find_top_row(tree, "tool.exe"), 0);
  CHECK(opens == 2);
  CHECK(previews == 0);
  CHECK(executes == 1);

  // Setting ON: previewable files preview, the rest fall back to OS-open,
  // executables still execute.
  s.set_double_clicks_open_previews(true);
  tab.on_item_double_clicked(find_top_row(tree, "readme.txt"), 0);
  tab.on_item_double_clicked(find_top_row(tree, "lib.dll"), 0);
  tab.on_item_double_clicked(find_top_row(tree, "tool.exe"), 0);
  CHECK(opens == 3);
  CHECK(previews == 1);
  CHECK(executes == 2);

  s.set_double_clicks_open_previews(false);
  std::filesystem::remove_all("/tmp/opencode/gmm_dblclick_datatab");
}
