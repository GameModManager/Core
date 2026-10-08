// GenericFilesTab (Text/Config Files tabs) — offscreen GUI regression test.
//
// Covers the wiring between the file list and the inline editor:
//   - set_mod() populates the list via a fresh QStandardItemModel, which swaps
//     in a new selection model. The ctor-time currentRowChanged connection is
//     orphaned unless rebuild_list() re-connects it — this test pins that the
//     editor actually loads the selected file's contents (the Aug 2026 bug).
//   - the Config Files tab picks up .ini/.cfg/.toml/.yaml/.yml/.json but never
//     the mod's own meta.ini; the Text Files tab keeps prose/scripts extensions
//     and must NOT list config extensions (dedupe).
//   - the filter box hides non-matching rows.
//
// Hermetic: offscreen platform, throwaway /tmp tree.
#include "ui/modinfo/config_files_tab.h"
#include "ui/modinfo/text_files_tab.h"
#include "ui/widgets/find_dialog.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
}  // namespace

static void write_file(const std::filesystem::path &path, const char *content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

// Collect the visible (un-hidden) file names from the list model.
static QStringList visible_names(QListView *list) {
  auto *model = qobject_cast<QStandardItemModel *>(list->model());
  if (!model)
    return {};
  QStringList names;
  for (int row = 0; row < model->rowCount(); ++row) {
    if (list->isRowHidden(row))
      continue;
    names.append(model->item(row)->text());
  }
  return names;
}

static bool contains(const QStringList &names, const char *needle) {
  return names.contains(QLatin1String(needle));
}

TEST_CASE("generic files tab", "[ui]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const std::filesystem::path base = "/tmp/gmm_generic_files";
  std::filesystem::remove_all(base);
  const std::filesystem::path mod_dir = base / "TestMod";
  std::filesystem::create_directories(mod_dir);
  write_file(mod_dir / "skee64.ini", "ini content\n");
  write_file(mod_dir / "readme.txt", "txt content\n");
  write_file(mod_dir / "settings.yaml", "yaml content\n");
  write_file(mod_dir / "plugin.esm", "esm content\n");
  write_file(mod_dir / "meta.ini", "manager data, not a game config\n");
  int test_argc     = 1;
  char test_argv0[] = "test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);

  ui::ModInfoData data;
  data.id      = "TestMod";
  data.name    = "Test Mod";
  data.mod_dir = QDir(QString::fromStdString(mod_dir.string()));

  // --- Config Files tab.
  {
    ui::ConfigFilesTab tab;
    tab.set_mod(data);
    auto *list   = tab.findChild<QListView *>();
    auto *editor = tab.findChild<QPlainTextEdit *>();
    check(list && editor, "Config Files tab exposes list and editor");
    if (list && editor) {
      QStringList names = visible_names(list);
      check(names.size() == 2 && contains(names, "skee64.ini") &&
                contains(names, "settings.yaml"),
            "Config Files lists .ini/.yaml and nothing else");
      check(!contains(names, "readme.txt") && !contains(names, "plugin.esm") &&
                !contains(names, "meta.ini"),
            "Config Files skips text/esm and the mod's own meta.ini");

      // Select the .ini row — editor must load its contents (the fix).
      auto *model         = qobject_cast<QStandardItemModel *>(list->model());
      QModelIndex ini_row = model->index(0, 0);
      for (int row = 0; row < model->rowCount(); ++row) {
        if (model->item(row)->text() == QLatin1String("skee64.ini")) {
          ini_row = model->index(row, 0);
          break;
        }
      }
      list->selectionModel()->setCurrentIndex(ini_row,
                                              QItemSelectionModel::ClearAndSelect);
      check(editor->isEnabled(), "selecting a config file enables the editor");
      check(editor->toPlainText() == QStringLiteral("ini content\n"),
            "editor shows the selected config file's contents");

      // --- Find: the dialog's status line is the only feedback a failed
      // search produces. Driven through the real button, the real dialog and
      // the real editor, so this asserts what the user would read.
      const auto buttons = tab.findChildren<QPushButton *>();
      QPushButton *find_button = nullptr;
      for (auto *btn : buttons) {
        if (btn->text().startsWith(QStringLiteral("Find"))) {
          find_button = btn;
          break;
        }
      }
      check(find_button != nullptr, "the editor bar exposes a Find button");
      if (find_button) {
        find_button->click();
        auto *dlg = tab.findChild<ui::FindDialog *>();
        auto *pattern = dlg ? dlg->findChild<QLineEdit *>() : nullptr;
        auto *status  = dlg ? dlg->findChild<QLabel *>() : nullptr;
        check(dlg && pattern && status,
              "Find opens a dialog carrying a pattern box and a status line");
        if (pattern && status) {
          pattern->setText(QStringLiteral("zzz-not-in-the-file"));
          check(status->text().contains(QStringLiteral("No results")),
                "a pattern that matches nothing says so instead of leaving the "
                "view silently unchanged");
          pattern->setText(QStringLiteral("ini"));
          check(status->text().isEmpty(),
                "a pattern that matches clears the failure notice");
          pattern->clear();
          check(status->text().isEmpty(),
                "emptying the box is not reported as a failed search");
        }
      }
    }
  }

  // --- Text Files tab.
  {
    ui::TextFilesTab tab;
    tab.set_mod(data);
    auto *list   = tab.findChild<QListView *>();
    auto *editor = tab.findChild<QPlainTextEdit *>();
    auto *filter = tab.findChild<QLineEdit *>();
    check(list && editor && filter, "Text Files tab exposes list, editor, filter");
    if (list && editor && filter) {
      QStringList names = visible_names(list);
      check(names.size() == 1 && contains(names, "readme.txt"),
            "Text Files lists readme.txt only");
      check(!contains(names, "skee64.ini") && !contains(names, "settings.yaml"),
            "Text Files no longer lists config extensions (dedupe)");
      check(!contains(names, "plugin.esm") && !contains(names, "meta.ini"),
            "Text Files skips .esm and meta.ini");

      // Filter hides non-matching rows.
      filter->setText(QStringLiteral("readme"));
      names = visible_names(list);
      check(names.size() == 1 && contains(names, "readme.txt"),
            "filter keeps the matching row");
      filter->setText(QStringLiteral("skee64"));
      names = visible_names(list);
      check(names.isEmpty(), "filter hides non-matching rows");

      // Selecting the remaining row loads the text file.
      filter->setText(QString());
      auto *model = qobject_cast<QStandardItemModel *>(list->model());
      list->selectionModel()->setCurrentIndex(model->index(0, 0),
                                              QItemSelectionModel::ClearAndSelect);
      check(editor->isEnabled() &&
                editor->toPlainText() == QStringLiteral("txt content\n"),
            "selecting a text file loads its contents into the editor");
    }
  }
}
