#include "ui/modinfo/generic_files_tab.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

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
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace ui {

GenericFilesTab::GenericFilesTab(QWidget* parent) : ModInfoTab(parent) {
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);

    auto* left = new QWidget(this);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    filter_ = new QLineEdit(left);
    filter_->setPlaceholderText(tr("Filter..."));
    left_layout->addWidget(filter_);

    list_ = new QListView(left);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    left_layout->addWidget(list_, 1);

    auto* right = new QWidget(this);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    editor_ = new QPlainTextEdit(right);
    QFont mono = editor_->font();
    mono.setFamily(QStringLiteral("monospace"));
    editor_->setFont(mono);
    editor_->setEnabled(false);
    right_layout->addWidget(editor_, 1);

    repository_ = new KSyntaxHighlighting::Repository;
    // SyntaxHighlighter is parented to the document (deleted with it).
    highlighter_ = new KSyntaxHighlighting::SyntaxHighlighter(editor_->document());

    auto* editor_bar = new QHBoxLayout();
    save_btn_ = new QPushButton(tr("Save"), right);
    save_btn_->setEnabled(false);
    editor_bar->addWidget(save_btn_);
    editor_bar->addStretch(1);
    right_layout->addLayout(editor_bar);

    splitter_->addWidget(left);
    splitter_->addWidget(right);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({200, 1});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter_);

    connect(filter_, &QLineEdit::textChanged, this, &GenericFilesTab::apply_filter);
    connect(list_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &GenericFilesTab::select_file);
    connect(save_btn_, &QPushButton::clicked, this, &GenericFilesTab::save_editor);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this]() {
        editor_dirty_ = editor_->isEnabled() &&
                        editor_->toPlainText() != last_loaded_text_;
        save_btn_->setEnabled(editor_dirty_);
    });
}

GenericFilesTab::~GenericFilesTab() {
    delete repository_;
}

void GenericFilesTab::set_mod(const ModInfoData& data) {
    files_.clear();
    editor_path_.clear();
    editor_->clear();
    editor_->setEnabled(false);
    save_btn_->setEnabled(false);
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
            const QString rel = root.relativeFilePath(full);
            if (wants_file(rel, full)) files_.push_back({full, rel});
        }
    }

    std::sort(files_.begin(), files_.end(),
              [](const File& a, const File& b) { return a.text < b.text; });
    set_has_data(!files_.empty());
    rebuild_list();
    apply_filter();
}

void GenericFilesTab::rebuild_list() {
    auto* model = new QStandardItemModel(this);
    for (const auto& f : files_) {
        model->appendRow(new QStandardItem(f.text));
    }
    list_->setModel(model);
    // setModel() swaps in a fresh selection model, which orphans the
    // constructor-time connection — re-connect so selecting a row loads the
    // file into the editor.
    connect(list_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &GenericFilesTab::select_file);
    list_->setEnabled(!files_.empty());
}

void GenericFilesTab::apply_filter() {
    const QString needle = filter_->text().trimmed();
    auto* model = qobject_cast<QStandardItemModel*>(list_->model());
    if (!model) return;
    for (int row = 0; row < model->rowCount(); ++row) {
        const bool visible =
            needle.isEmpty() ||
            model->item(row)->text().contains(needle, Qt::CaseInsensitive);
        list_->setRowHidden(row, !visible);
    }
}

void GenericFilesTab::select_file(const QModelIndex& index) {
    if (!index.isValid()) return;
    if (!maybe_flush_editor()) return;
    load_editor(files_[static_cast<size_t>(index.row())].full_path);
}

void GenericFilesTab::load_editor(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Open File"),
                             tr("Could not open \"%1\".").arg(path));
        return;
    }
    editor_path_ = path;
    last_loaded_text_ = QString::fromUtf8(f.readAll());
    editor_->setPlainText(last_loaded_text_);
    // Filename-based grammar selection covers ini/cfg/toml/yaml/json/xml/...
    // for free; unknown extensions fall back to plain text (invalid Definition
    // clears highlighting).
    highlighter_->setDefinition(
        repository_->definitionForFileName(QFileInfo(path).fileName()));
    apply_theme();
    editor_->setEnabled(true);
    editor_dirty_ = false;
    save_btn_->setEnabled(false);
}

// Picks a KSyntaxHighlighting theme that matches the editor's palette so the
// highlighted text stays readable in both the light and dark app themes. Runs
// per file load and on application palette changes (live theme switching).
void GenericFilesTab::apply_theme() {
    if (!highlighter_) return;
    const KSyntaxHighlighting::Theme theme =
        repository_->themeForPalette(editor_->palette());
    if (theme.isValid()) {
        highlighter_->setTheme(theme);
        highlighter_->rehighlight();
    }
}

bool GenericFilesTab::event(QEvent* event) {
    if (event->type() == QEvent::ApplicationPaletteChange) apply_theme();
    return ModInfoTab::event(event);
}

void GenericFilesTab::save_editor() {
    if (editor_path_.isEmpty()) return;
    QFile f(editor_path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save File"),
                             tr("Could not write \"%1\".").arg(editor_path_));
        return;
    }
    const QString text = editor_->toPlainText();
    f.write(text.toUtf8());
    last_loaded_text_ = text;
    editor_dirty_ = false;
    save_btn_->setEnabled(false);
}

bool GenericFilesTab::maybe_flush_editor() {
    if (!editor_dirty_) return true;
    const int res = QMessageBox::question(
        this, tr("Save Changes"),
        tr("Save changes to \"%1\"?").arg(editor_path_),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (res == QMessageBox::Save) save_editor();
    return res != QMessageBox::Cancel;
}

void GenericFilesTab::save_state() {
    maybe_flush_editor();
}

bool GenericFilesTab::can_close() {
    return maybe_flush_editor();
}

}  // namespace ui
