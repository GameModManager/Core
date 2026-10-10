#include "ui/modinfo/generic_files_tab.h"

#include "ui/widgets/find_dialog.h"
#include "ui/widgets/line_number_edit.h"

#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif

#include <QDesktopServices>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

GenericFilesTab::GenericFilesTab(QWidget *parent) : ModInfoTab(parent) {
  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->setChildrenCollapsible(false);

  auto *left        = new QWidget(this);
  auto *left_layout = new QVBoxLayout(left);
  left_layout->setContentsMargins(0, 0, 0, 0);
  filter_ = new QLineEdit(left);
  filter_->setPlaceholderText(tr("Filter..."));
  left_layout->addWidget(filter_);

  list_ = new QListView(left);
  list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  left_layout->addWidget(list_, 1);

  auto *right        = new QWidget(this);
  auto *right_layout = new QVBoxLayout(right);
  right_layout->setContentsMargins(0, 0, 0, 0);
  editor_    = new LineNumberPlainTextEdit(right);
  QFont mono = editor_->font();
  mono.setFamily(QStringLiteral("monospace"));
  editor_->setFont(mono);
  editor_->setEnabled(false);
  right_layout->addWidget(editor_, 1);

#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
  repository_ = new KSyntaxHighlighting::Repository;
  // SyntaxHighlighter is parented to the document (deleted with it).
  highlighter_ = new KSyntaxHighlighting::SyntaxHighlighter(editor_->document());
#endif

  auto *editor_bar = new QHBoxLayout();
  save_btn_        = new QPushButton(tr("Save"), right);
  save_btn_->setEnabled(false);
  editor_bar->addWidget(save_btn_);
  auto *find_btn = new QPushButton(tr("Find..."), right);
  find_btn->setToolTip(tr("Search this file for text (Ctrl+F)"));
  find_btn->setEnabled(false);
  find_btn_ = find_btn;
  editor_bar->addWidget(find_btn);

  // MO2 TextEditorToolbar's Word wrap (texteditor.cpp:473-475): checkable,
  // and checked exactly when the editor is wrapping, so the button states the
  // mode rather than flipping a blind copy of it.
  wrap_btn_ = new QPushButton(tr("Word Wrap"), right);
  wrap_btn_->setCheckable(true);
  wrap_btn_->setToolTip(tr("Wrap long lines at the edge of the editor"));
  wrap_btn_->setChecked(editor_->lineWrapMode() == QPlainTextEdit::WidgetWidth);
  editor_bar->addWidget(wrap_btn_);

  // MO2's Open in Explorer (texteditor.cpp:477), renamed for a cross-platform
  // app: it opens the containing folder of the loaded file, which is what a
  // file manager shows either way.
  explore_btn_ = new QPushButton(tr("Open in File Manager"), right);
  explore_btn_->setToolTip(tr("Show this file in the system file manager"));
  explore_btn_->setEnabled(false);
  editor_bar->addWidget(explore_btn_);

  editor_bar->addStretch(1);
  right_layout->addLayout(editor_bar);

  // Ctrl+F on the tab, scoped to it and its children: a window-scoped pair
  // would also claim the shortcut on every other tab of the mod info dialog.
  auto *find_shortcut = new QShortcut(QKeySequence::Find, this);
  find_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
  connect(find_shortcut, &QShortcut::activated, this,
          &GenericFilesTab::open_find_dialog);
  connect(find_btn, &QPushButton::clicked, this, &GenericFilesTab::open_find_dialog);

  splitter_->addWidget(left);
  splitter_->addWidget(right);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  splitter_->setSizes({200, 1});

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(splitter_);

  connect(filter_, &QLineEdit::textChanged, this, &GenericFilesTab::apply_filter);
  // NOTE: no selectionModel connection here - QListView has no model yet
  // so selectionModel() is nullptr (QObject::connect nullptr warning).
  // rebuild_list() connects after setModel() swaps in a fresh one.
  connect(save_btn_, &QPushButton::clicked, this, &GenericFilesTab::save_editor);
  connect(wrap_btn_, &QPushButton::toggled, this, [this](bool on) {
    // MO2 TextEditor::toggleWordWrap (texteditor.cpp:142-145).
    editor_->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
  });
  connect(explore_btn_, &QPushButton::clicked, this,
          &GenericFilesTab::open_in_file_manager);
  connect(editor_, &QPlainTextEdit::textChanged, this, [this]() {
    editor_dirty_ = editor_->isEnabled() && editor_->toPlainText() != last_loaded_text_;
    save_btn_->setEnabled(editor_dirty_);
    find_btn_->setEnabled(editor_->isEnabled());
  });
}

GenericFilesTab::~GenericFilesTab() {
#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
  delete repository_;
#endif
}

void GenericFilesTab::set_mod(const ModInfoData &data) {
  files_.clear();
  editor_path_.clear();
  editor_->clear();
  editor_->setEnabled(false);
  save_btn_->setEnabled(false);
  find_btn_->setEnabled(false);
  explore_btn_->setEnabled(false);
  editor_dirty_ = false;
  last_loaded_text_.clear();

  // The mod folder root IS the game-data root (MO2 layout); data_dir()
  // appends mods_subpath ("Data") which mods never contain.
  const QDir root = data.mod_dir;
  if (root.exists()) {
    QDirIterator it(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QString full = it.next();
      const QString rel  = root.relativeFilePath(full);
      if (wants_file(rel, full))
        files_.push_back({full, rel});
    }
  }

  std::sort(files_.begin(), files_.end(), [](const File &a, const File &b) {
    return a.text < b.text;
  });
  set_has_data(!files_.empty());
  rebuild_list();
  apply_filter();
}

void GenericFilesTab::rebuild_list() {
  // The previous model is parented to this tab, so it would linger as a
  // child until the tab is destroyed - delete it before swapping in the
  // new one (QListView::setModel takes no ownership).
  if (list_->model() && list_->model()->parent() == this) {
    delete list_->model();
  }
  auto *model = new QStandardItemModel(this);
  for (const auto &f : files_) {
    model->appendRow(new QStandardItem(f.text));
  }
  list_->setModel(model);
  // setModel() swaps in a fresh selection model, which orphans the
  // previous connection - re-connect so selecting a row loads the
  // file into the editor. Guard: selectionModel() can be null if the
  // model failed to attach.
  if (auto *selection = list_->selectionModel()) {
    connect(selection, &QItemSelectionModel::currentRowChanged, this,
            &GenericFilesTab::select_file, Qt::UniqueConnection);
  }
  list_->setEnabled(!files_.empty());
}

void GenericFilesTab::apply_filter() {
  const QString needle = filter_->text().trimmed();
  auto *model          = qobject_cast<QStandardItemModel *>(list_->model());
  if (!model)
    return;
  for (int row = 0; row < model->rowCount(); ++row) {
    const bool visible = needle.isEmpty() ||
                         model->item(row)->text().contains(needle, Qt::CaseInsensitive);
    list_->setRowHidden(row, !visible);
  }
}

void GenericFilesTab::select_file(const QModelIndex &index) {
  if (!index.isValid())
    return;
  if (!maybe_flush_editor())
    return;
  load_editor(files_[static_cast<size_t>(index.row())].full_path);
}

void GenericFilesTab::load_editor(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QMessageBox::warning(this, tr("Open File"), tr("Could not open \"%1\".").arg(path));
    return;
  }
  editor_path_      = path;
  last_loaded_text_ = QString::fromUtf8(f.readAll());
  editor_->setPlainText(last_loaded_text_);
#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
  // Filename-based grammar selection covers ini/cfg/toml/yaml/json/xml/...
  // for free; unknown extensions fall back to plain text (invalid Definition
  // clears highlighting).
  highlighter_->setDefinition(
      repository_->definitionForFileName(QFileInfo(path).fileName()));
#endif
  apply_theme();
  editor_->setEnabled(true);
  editor_dirty_ = false;
  save_btn_->setEnabled(false);
  find_btn_->setEnabled(true);
  explore_btn_->setEnabled(true);
}

void GenericFilesTab::open_in_file_manager() {
  if (editor_path_.isEmpty())
    return;
  // MO2 hands the FILE to shell::Explore, which selects it inside its folder.
  // GMM opens the containing folder instead: QDesktopServices has no
  // cross-platform "reveal", and the file is already named in the list the
  // action was taken from.
  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QFileInfo(editor_path_).absolutePath()));
}

// Picks a KSyntaxHighlighting theme that matches the editor's palette so the
// highlighted text stays readable in both the light and dark app themes. Runs
// per file load and on application palette changes (live theme switching).
// No-op when KF6 is unavailable (plain-text editor).
void GenericFilesTab::apply_theme() {
#ifdef GMM_HAS_SYNTAX_HIGHLIGHTING
  if (!highlighter_)
    return;
  const KSyntaxHighlighting::Theme theme =
      repository_->themeForPalette(editor_->palette());
  if (theme.isValid()) {
    highlighter_->setTheme(theme);
    highlighter_->rehighlight();
  }
#endif
}

void GenericFilesTab::open_find_dialog() {
  if (!editor_->isEnabled())
    return;
  if (!find_dlg_) {
    find_dlg_ = new FindDialog(this);
    // patternChanged is the live half: typing in the box lands on the first
    // match at once, which is what makes a find box feel like find and not
    // like a form with a submit button.
    connect(find_dlg_, &FindDialog::patternChanged, this,
            [this](const QString &pattern) {
              find_next(pattern, find_dlg_->case_sensitive(), 0);
            });
    connect(find_dlg_, &FindDialog::findNext, this, [this]() {
      // -1 asks the editor to continue from wherever it is, so Find Next
      // advances instead of restarting the search at the top.
      find_next(find_dlg_->pattern(), find_dlg_->case_sensitive(), -1);
    });
  }
  find_dlg_->show();
  find_dlg_->raise();
  find_dlg_->activateWindow();
}

bool GenericFilesTab::find_next(const QString &pattern, bool case_sensitive, int from) {
  if (pattern.isEmpty()) {
    // An emptied box is not a failed search: it clears the stale "no results"
    // from the previous pattern instead of reporting one.
    if (find_dlg_)
      find_dlg_->set_status(QString());
    return false;
  }
  const QTextCursor cursor = editor_->textCursor();
  // Resume past the hit the last search landed on, so Find Next advances
  // instead of re-finding it; `from` is for the caller that wants a specific
  // spot (the live search as the user types, which restarts at the top).
  const int start =
      from >= 0 ? from
                : (cursor.hasSelection() ? cursor.selectionEnd() : cursor.position());
  // No flag is case-INsensitive, which is the default worth having here.
  QTextDocument::FindFlags flags;
  if (case_sensitive)
    flags |= QTextDocument::FindCaseSensitively;

  QTextCursor found = editor_->document()->find(pattern, start, flags);
  if (found.isNull() && start > 0) {
    // Past the last hit: wrap, so Find Next cycles. Stalling at the end of the
    // file reads as a failed search rather than as "wrapped".
    found = editor_->document()->find(pattern, 0, flags);
  }
  if (found.isNull()) {
    // Say so. The dialog cannot search, so this is the only place the outcome
    // is known, and with nothing written the view just sat there unchanged -
    // which reads as a broken Find Next rather than as an exhausted search.
    if (find_dlg_)
      find_dlg_->set_status(tr("No results"));
    return false;
  }
  editor_->setTextCursor(found);
  editor_->ensureCursorVisible();
  editor_->setFocus();
  if (find_dlg_)
    find_dlg_->set_status(QString());
  return true;
}

bool GenericFilesTab::event(QEvent *event) {
  if (event->type() == QEvent::ApplicationPaletteChange)
    apply_theme();
  return ModInfoTab::event(event);
}

void GenericFilesTab::save_editor() {
  if (editor_path_.isEmpty())
    return;
  QFile f(editor_path_);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    QMessageBox::warning(this, tr("Save File"),
                         tr("Could not write \"%1\".").arg(editor_path_));
    return;
  }
  const QString text = editor_->toPlainText();
  f.write(text.toUtf8());
  last_loaded_text_ = text;
  editor_dirty_     = false;
  save_btn_->setEnabled(false);
}

bool GenericFilesTab::maybe_flush_editor() {
  if (!editor_dirty_)
    return true;
  const int res = QMessageBox::question(
      this, tr("Save Changes"), tr("Save changes to \"%1\"?").arg(editor_path_),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (res == QMessageBox::Save)
    save_editor();
  return res != QMessageBox::Cancel;
}

void GenericFilesTab::save_state() {
  maybe_flush_editor();
}

bool GenericFilesTab::can_close() {
  return maybe_flush_editor();
}

}  // namespace ui
