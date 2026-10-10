#pragma once

#include "ui/modinfo/mod_info_tab.h"

#include <QString>

#include <vector>

// LineNumberPlainTextEdit is a concrete type used only as a pointer member;
// a forward declaration suffices here.

class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;

#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
namespace KSyntaxHighlighting {
class Repository;
class SyntaxHighlighter;
}  // namespace KSyntaxHighlighting
#endif

namespace ui {

class FindDialog;
class LineNumberPlainTextEdit;

// MO2's GenericFilesTab: a filterable list of files (matched by a subclass
// predicate) on the left and an inline plain-text editor on the right. Dirty
// edits are flushed on mod switch / dialog close (canClose prompts). Used by
// both the Text Files and Config Files tabs. The editor gets syntax
// highlighting via KSyntaxHighlighting when available
// (GMM_HAS_SYNTAX_HIGHLIGHTING), resolved per file from its file name;
// otherwise it stays a plain text editor (no KF6 exists for Windows).
class GenericFilesTab : public ModInfoTab {
  Q_OBJECT
public:
  explicit GenericFilesTab(QWidget *parent = nullptr);
  ~GenericFilesTab() override;

  void set_mod(const ModInfoData &data) override;
  void save_state() override;
  bool can_close() override;

protected:
  // Return true to include `full_path` (path relative to the mod's data dir
  // is also given for cheap extension checks).
  virtual bool wants_file(const QString &rel_path, const QString &full_path) const = 0;

  bool event(QEvent *event) override;

private:
  struct File {
    QString full_path;
    QString text;
  };

  void rebuild_list();
  void apply_filter();
  void select_file(const QModelIndex &index);
  void load_editor(const QString &path);
  void save_editor();
  // MO2 TextEditor::explore (texteditor.cpp:203-210): reveal the loaded file
  // in the system file manager. No-op with no file loaded.
  void open_in_file_manager();
  bool maybe_flush_editor();
  void apply_theme();
  // Find-in-text (MO2's MOBase::FindDialog). Ctrl+F opens the dialog; the
  // tab does the searching, because it is the one that owns the editor.
  void open_find_dialog();
  // Search from `from` (or the dialog's origin when from < 0) and select the
  // hit. Returns true when something matched, so the dialog can say so.
  bool find_next(const QString &pattern, bool case_sensitive, int from);

  QSplitter *splitter_             = nullptr;
  QListView *list_                 = nullptr;
  QLineEdit *filter_               = nullptr;
  LineNumberPlainTextEdit *editor_ = nullptr;
  QPushButton *save_btn_           = nullptr;
  // Enabled exactly when there is a file loaded to search; open_find_dialog
  // also refuses with no file, so the two agree.
  QPushButton *find_btn_ = nullptr;
  // MO2 TextEditorToolbar (texteditor.cpp:471-477): the word-wrap action is
  // checkable and shows the editor's current wrap mode, "Open in Explorer"
  // lives beside it. Both are per-file tools on the same bar as Save.
  QPushButton *wrap_btn_    = nullptr;
  QPushButton *explore_btn_ = nullptr;
  // Built on the first Ctrl+F, reused after that: a fresh dialog each time
  // would lose the pattern the user is stepping through.
  FindDialog *find_dlg_ = nullptr;
#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
  KSyntaxHighlighting::Repository *repository_         = nullptr;
  KSyntaxHighlighting::SyntaxHighlighter *highlighter_ = nullptr;
#endif
  std::vector<File> files_;
  QString editor_path_;
  QString last_loaded_text_;
  bool editor_dirty_ = false;
};

}  // namespace ui
