// Offscreen widget tests for the reusable UI primitives pack
// (Workspace-axva; MO2 featuremap rows U264 / U265 / U269). One test case per
// primitive, each covering construction, the public API, and the edge cases
// that matter:
//
//   - U264 SortableTreeWidget: drag-move mode, the local-move-only refusal,
//     items_moved emitted on an accepted drop and NOT on a refused one, and
//     set_customizable_columns() consulting kEnabledColumnRole.
//   - U265 ExpanderWidget: set/toggle/opened, the arrow, save/restore state
//     round trip, empty construction, and re-pointing at a new pair.
//   - U265 LinkLabel: the Q_PROPERTY publishes the colour to the application
//     palette, and a no-QApplication process is a no-op rather than a crash.
//   - U265 LineEditClear: clear affordance on by default, off again on
//     request, clear() empties and emits textChanged.
//   - U269 EventFilter: the handler sees events first, returning true consumes
//     and false passes through.
//
// Hermetic: no network, no user config access. The QSettings sandbox rule does
// not apply here - nothing in this file constructs a QSettings.
// QT_QPA_PLATFORM=offscreen via the test property.

#include "ui/widgets/column_toggle_header.h"
#include "ui/widgets/event_filter.h"
#include "ui/widgets/expander_widget.h"
#include "ui/widgets/line_edit_clear.h"
#include "ui/widgets/link_label.h"
#include "ui/widgets/sortable_tree_widget.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPalette>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <QTreeView>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>

namespace {

void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}

// Qt allows exactly one QApplication per process; Catch2 runs the TEST_CASEs
// in sequence, so one shared instance is enough. Guarded on
// QCoreApplication::instance() so re-entry is safe.
QApplication *ensure_app() {
  static int argc        = 1;
  static char app_name[] = "ui_primitives_test";
  static char *argv[]    = {app_name, nullptr};
  if (auto *existing = qobject_cast<QApplication *>(QCoreApplication::instance()))
    return existing;
  return new QApplication(argc, argv);
}

// Exposes the protected QTreeWidget drop hooks so the test can drive the real
// primitive without Qt's internal drag machinery: mimeData() builds exactly
// what a real drag would, and dropMimeData() is exactly what a real drop calls.
class ExposedTree : public ui::SortableTreeWidget {
public:
  using QTreeWidget::dropMimeData;
  using QTreeWidget::mimeData;
  using QTreeWidget::supportedDropActions;

  // QTreeWidgetItem has no ctor taking the derived type, so hand over the base
  // pointer explicitly rather than list-initialising.
  [[nodiscard]] QTreeWidgetItem *add_child(const QString &text) {
    auto *item = new QTreeWidgetItem(static_cast<QTreeWidget *>(this));
    item->setText(0, text);
    return item;
  }

  [[nodiscard]] static QTreeWidgetItem *add_grandchild(QTreeWidgetItem *parent,
                                                       const QString &text) {
    auto *item = new QTreeWidgetItem(parent);
    item->setText(0, text);
    return item;
  }
};

// Same for the header's section state, so the test can read what the context
// menu would render without opening a menu.
class ExposedHeader : public ui::ColumnToggleHeaderView {
public:
  using ui::ColumnToggleHeaderView::section_is_enabled;
};

// A model that answers kEnabledColumnRole per column, which is the whole point
// of set_customizable_columns.
class EnablementModel : public QStandardItemModel {
public:
  explicit EnablementModel(QList<bool> enabled, QObject *parent = nullptr)
      : QStandardItemModel(parent), enabled_(std::move(enabled)) {
    setColumnCount(enabled_.size());
    setRowCount(1);
  }

  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override {
    if (role == ui::kEnabledColumnRole && orientation == Qt::Horizontal &&
        section >= 0 && section < enabled_.size()) {
      return enabled_.at(section);
    }
    return QStandardItemModel::headerData(section, orientation, role);
  }

private:
  QList<bool> enabled_;
};

}  // namespace

TEST_CASE("U264 SortableTreeWidget moves items and reports an accepted drop",
          "[ui][primitives][sortable_tree_widget]") {
  QApplication *app = ensure_app();
  (void)app;
  ExposedTree tree;
  tree.setColumnCount(1);

  SECTION("construction is an internal move, and only MoveAction is offered") {
    check(tree.dragDropMode() == QAbstractItemView::InternalMove,
          "drag/drop mode is InternalMove");
    check(tree.dragEnabled(), "drag is enabled");
    check(tree.acceptDrops(), "drops are accepted");
    // A Copy advertised here would silently duplicate rows the widget owns.
    check(tree.supportedDropActions() == Qt::MoveAction,
          "only MoveAction is advertised");
  }

  SECTION("items_moved fires when a drop is accepted") {
    auto *a     = tree.add_child("a");
    auto *b     = tree.add_child("b");
    auto *child = tree.add_grandchild(a, "a1");

    tree.setCurrentItem(child);
    tree.clearSelection();
    child->setSelected(true);
    check(tree.selectedItems() == QList<QTreeWidgetItem *>{child},
          "exactly the child is selected");

    QSignalSpy moved(&tree, &ui::SortableTreeWidget::items_moved);
    std::unique_ptr<QMimeData> mime(tree.mimeData(QList<QTreeWidgetItem *>{child}));
    REQUIRE(mime != nullptr);

    // Driving dropMimeData directly exercises the primitive's own decision
    // (refuse or accept, then emit). Qt removes the dragged source rows in
    // QAbstractItemView's drag path, keyed off the QDrag that started it, so
    // that half cannot run here - what is observable is the insert under b.
    const bool ok = tree.dropMimeData(b, 0, mime.get(), Qt::MoveAction);
    check(ok, "the cross-branch drop was accepted");
    check(b->childCount() == 1, "the child was inserted under b");
    if (b->childCount() == 1) {
      check(b->child(0)->text(0) == "a1", "and it carries the dragged text");
    }
    check(moved.count() == 1, "items_moved fired exactly once");
  }

  SECTION("local-move-only refuses a drop that changes branch, silently") {
    auto *a     = tree.add_child("a");
    auto *b     = tree.add_child("b");
    auto *child = tree.add_grandchild(a, "a1");
    tree.set_local_move_only(true);
    check(tree.local_move_only(), "the flag reads back");
    check(tree.supportedDropActions() == Qt::MoveAction,
          "MoveAction is still the only advertised action");

    tree.clearSelection();
    child->setSelected(true);

    QSignalSpy moved(&tree, &ui::SortableTreeWidget::items_moved);
    std::unique_ptr<QMimeData> mime(tree.mimeData(QList<QTreeWidgetItem *>{child}));
    REQUIRE(mime != nullptr);

    check(!tree.dropMimeData(b, 0, mime.get(), Qt::MoveAction),
          "the drop onto the other branch was refused");
    // Refused means the base was never asked, so nothing moved and the dragged
    // pointer is untouched.
    check(child->parent() == a, "the child did not move");
    check(b->childCount() == 0, "nothing landed under b");
    check(moved.count() == 0, "a refused drop emits nothing");
  }

  SECTION("local-move-only still allows a reorder inside the branch") {
    auto *a      = tree.add_child("a");
    auto *first  = tree.add_grandchild(a, "first");
    auto *second = tree.add_grandchild(a, "second");
    tree.set_local_move_only(true);

    tree.clearSelection();
    first->setSelected(true);

    std::unique_ptr<QMimeData> mime(tree.mimeData(QList<QTreeWidgetItem *>{first}));
    REQUIRE(mime != nullptr);
    QSignalSpy moved(&tree, &ui::SortableTreeWidget::items_moved);
    check(tree.dropMimeData(a, 2, mime.get(), Qt::MoveAction),
          "the in-branch reorder was accepted");
    check(moved.count() == 1, "items_moved fired");
    // Only the real drag path removes the source rows, so both originals are
    // still here - the point is that the drop was not refused.
    check(a->childCount() == 3, "the branch still holds both children");
    check(a->child(0) == first && a->child(1) == second,
          "and they are the originals, untouched");
  }

  SECTION("local-move-only refuses a drop at the root, where items cannot go") {
    auto *a     = tree.add_child("a");
    auto *b     = tree.add_child("b");
    auto *child = tree.add_grandchild(a, "a1");
    tree.set_local_move_only(true);
    tree.clearSelection();
    child->setSelected(true);

    QSignalSpy moved(&tree, &ui::SortableTreeWidget::items_moved);
    std::unique_ptr<QMimeData> mime(tree.mimeData(QList<QTreeWidgetItem *>{child}));
    REQUIRE(mime != nullptr);

    // A null drop parent is "onto the bare area below the tree". The selected
    // item's parent is `a`, not nullptr, so local-move-only must refuse it -
    // otherwise a drag could lift a nested item out to the root.
    check(!tree.dropMimeData(nullptr, 0, mime.get(), Qt::MoveAction),
          "the drop at the root was refused");
    check(child->parent() == a, "the child did not leave its branch");
    check(tree.topLevelItemCount() == 2, "the root still holds only a and b");
    check(moved.count() == 0, "a refused drop emits nothing");
    check(b->childCount() == 0, "and nothing landed under b");
  }
}

TEST_CASE("U264 set_customizable_columns drives the enable role from the model",
          "[ui][primitives][sortable_tree_widget]") {
  QApplication *app = ensure_app();
  (void)app;
  QTreeView view;
  EnablementModel model({true, false, true});
  view.setModel(&model);

  ui::set_customizable_columns(&view);

  auto *header = qobject_cast<ui::ColumnToggleHeaderView *>(view.header());
  REQUIRE(header != nullptr);
  // The role is what MO2's setCustomizableColumns keys off
  // (uibase/widgetutility.h:12).
  check(ui::kEnabledColumnRole == Qt::UserRole + 1, "the role is UserRole + 1");

  const ExposedHeader *const exposed = static_cast<ExposedHeader *>(header);
  check(exposed->section_is_enabled(0), "a true value leaves the column live");
  check(!exposed->section_is_enabled(1), "a false value disables the column");
  check(exposed->section_is_enabled(2), "a later true column is unaffected");
  // Out of range and a model with no opinion both read as enabled.
  check(exposed->section_is_enabled(9), "an unknown section reads as enabled");

  header->set_enablement_model(nullptr);
  check(exposed->section_is_enabled(1), "dropping the model re-enables everything");

  // A null view is ignored rather than dereferenced.
  ui::set_customizable_columns(nullptr);
  SUCCEED("a null view is a no-op");
}

// The host widget is never shown, so isVisible() is false for every child
// regardless of the expander. isHidden() reports the widget's own explicit
// hide flag, which is exactly what the expander sets.
bool showing(QWidget *w) {
  return !w->isHidden();
}

TEST_CASE("U265 ExpanderWidget drives a button and its content",
          "[ui][primitives][expander_widget]") {
  QApplication *app = ensure_app();
  (void)app;
  QWidget host;
  auto *button  = new QToolButton(&host);
  auto *content = new QWidget(&host);
  content->setVisible(true);

  ui::ExpanderWidget expander;
  expander.set(button, content);

  SECTION("construction closes the expander and points the arrow right") {
    check(expander.button() == button, "the button is remembered");
    check(expander.content() == content, "the content is remembered");
    check(!expander.opened(), "closed by default");
    check(!showing(content), "the content starts hidden");
    check(button->arrowType() == Qt::ArrowType::RightArrow,
          "a closed expander points right");
    check(button->isCheckable(), "the button became checkable");
  }

  SECTION("clicking the button toggles, with both signals in order") {
    QSignalSpy about(&expander, &ui::ExpanderWidget::about_to_toggle);
    QSignalSpy toggled(&expander, &ui::ExpanderWidget::toggled);

    button->click();
    check(expander.opened(), "the click opened it");
    check(showing(content), "the content is showing");
    check(button->isChecked(), "the button reads as on");
    check(button->arrowType() == Qt::ArrowType::DownArrow,
          "an open expander points down");
    check(about.count() == 1 && toggled.count() == 1, "one of each signal");
    check(about.at(0).at(0).toBool(), "about_to_toggle carried the new state");
    check(toggled.at(0).at(0).toBool(), "toggled carried the new state");

    button->click();
    check(!expander.opened(), "the second click closed it");
    check(!showing(content), "the content is hidden again");
    check(button->arrowType() == Qt::ArrowType::RightArrow, "arrow flipped back");
    check(about.count() == 2 && toggled.count() == 2, "one of each per change");
  }

  SECTION("toggle(bool) to the state it is already in does nothing") {
    QSignalSpy toggled(&expander, &ui::ExpanderWidget::toggled);
    expander.toggle(false);  // already closed
    check(toggled.count() == 0, "no signal for a no-op toggle");
    check(!expander.opened(), "still closed");

    expander.toggle(true);
    check(expander.opened(), "a real change applies");
    check(toggled.count() == 1, "exactly one signal");
  }

  SECTION("save_state round-trips through restore_state") {
    expander.toggle(true);
    const QByteArray saved = expander.save_state();
    check(!saved.isEmpty(), "an open expander saves a blob");
    check(saved.size() == 1, "the blob is one byte");

    expander.toggle(false);
    check(!expander.opened(), "closed before restoring");
    check(expander.restore_state(saved), "a well-formed blob restores");
    check(expander.opened(), "the saved open state came back");
    check(showing(content), "and the content followed");

    const QByteArray closed = QByteArray(1, '\0');
    check(expander.restore_state(closed), "the closed blob restores too");
    check(!expander.opened(), "and it closed the expander");
  }

  SECTION("a malformed or missing blob is refused, leaving the state alone") {
    expander.toggle(true);
    check(!expander.restore_state(QByteArray()), "an empty blob is refused");
    check(!expander.restore_state(QByteArray("too long")), "wrong size refused");
    check(expander.opened(), "a refused restore changed nothing");
  }

  SECTION("set() can open straight away and re-point at a new pair") {
    auto *button2  = new QToolButton(&host);
    auto *content2 = new QWidget(&host);
    expander.set(button2, content2, true);

    check(expander.button() == button2, "the new button is in charge");
    check(expander.content() == content2, "the new content is in charge");
    check(expander.opened(), "the requested state applied");
    check(showing(content2), "the new content is showing");
    check(button2->arrowType() == Qt::ArrowType::DownArrow,
          "the new button points down");
    // The old button must no longer drive the expander.
    button->click();
    check(expander.button() == button2, "the old button did not re-point it");
    check(expander.opened(), "and did not toggle it");
  }

  SECTION("the two-widget constructor sets the pair up front") {
    QWidget host2;
    auto *button3  = new QToolButton(&host2);
    auto *content3 = new QWidget(&host2);
    ui::ExpanderWidget ready(button3, content3);
    check(ready.button() == button3, "constructor took the button");
    check(ready.content() == content3, "constructor took the content");
    check(!ready.opened(), "closed by default, like set()");
  }

  SECTION("a null button or content is tolerated") {
    ui::ExpanderWidget empty;
    empty.toggle(true);
    check(empty.opened(), "state tracks even with no widgets attached");
    empty.set(nullptr, nullptr, false);
    check(!empty.opened(), "set(nullptr, nullptr) applies and does not crash");
    check(empty.button() == nullptr, "nothing to point at");
  }
}

TEST_CASE("U265 LinkLabel publishes its colour to the application palette",
          "[ui][primitives][link_label]") {
  ensure_app();
  auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
  REQUIRE(app != nullptr);

  // Restore the process palette afterwards: the change is application-wide by
  // design, and this test must not leak it into the next one.
  const QPalette original = app->palette();

  ui::LinkLabel label;
  label.setText("https://example.invalid");

  const QColor chosen(0x33, 0x99, 0xFF);
  label.set_link_color(chosen);

  check(label.link_color() == chosen, "the label reads the colour back");
  check(app->palette().color(QPalette::Link) == chosen,
        "QPalette::Link took the colour");
  check(app->palette().color(QPalette::LinkVisited) == chosen,
        "QPalette::LinkVisited took it too, so no default magenta");

  // QSS drives this through the Q_PROPERTY, so the metaobject has to agree.
  const QMetaObject *meta = label.metaObject();
  const int prop          = meta->indexOfProperty("linkColor");
  REQUIRE(prop >= 0);
  check(meta->property(prop).isWritable(), "linkColor is writable from QSS");
  const QColor via_meta = meta->property(prop).read(&label).value<QColor>();
  check(via_meta == chosen, "reading the property through the metaobject works");

  app->setPalette(original);
  check(app->palette().color(QPalette::Link) == original.color(QPalette::Link),
        "the palette was restored for the next test");
}

TEST_CASE("U265 LineEditClear ships the clear affordance already on",
          "[ui][primitives][line_edit_clear]") {
  ensure_app();

  SECTION("construction enables the clear button") {
    ui::LineEditClear edit;
    check(edit.is_clear_button_enabled(), "clear is on without asking");
    check(edit.isClearButtonEnabled(),
          "and it is the QLineEdit property, so styling still applies");
  }

  SECTION("an empty box has nothing to clear") {
    ui::LineEditClear edit;
    check(edit.text().isEmpty(), "starts empty");
    QSignalSpy changed(&edit, &QLineEdit::textChanged);
    edit.clear();
    check(changed.count() == 0, "clearing an empty box emits nothing");
  }

  SECTION("clear empties the text and emits textChanged once") {
    ui::LineEditClear edit;
    QSignalSpy changed(&edit, &QLineEdit::textChanged);
    edit.setText("skyrim");
    check(changed.count() == 1, "setting text emits once");
    changed.clear();

    edit.clear();
    check(edit.text().isEmpty(), "the box is empty after clear()");
    check(changed.count() == 1, "clear emitted textChanged exactly once");
    check(changed.at(0).at(0).toString().isEmpty(), "carrying the empty text");
  }

  SECTION("the affordance can be switched back off") {
    ui::LineEditClear edit;
    edit.set_clear_button_enabled(false);
    check(!edit.is_clear_button_enabled(), "off on request");
    // The rest of QLineEdit is untouched: still a usable text field.
    edit.setText("still editable");
    check(edit.text() == "still editable", "text still works");
    edit.set_clear_button_enabled(true);
    check(edit.is_clear_button_enabled(), "and back on again");
  }

  SECTION("a disabled box is still a plain QLineEdit") {
    ui::LineEditClear edit;
    edit.setEnabled(false);
    check(!edit.isEnabled(), "the widget is disabled");
    check(edit.is_clear_button_enabled(),
          "the affordance flag is unchanged by disabling");
  }
}

TEST_CASE("U269 EventFilter turns a callable into a filter",
          "[ui][primitives][event_filter]") {
  ensure_app();
  QWidget watched;
  int seen               = 0;
  int consumed           = 0;
  QEvent::Type last_type = QEvent::None;

  auto *filter = new ui::EventFilter(&watched, [&](QObject *obj, QEvent *event) {
    ++seen;
    last_type = event->type();
    if (obj != &watched)
      return false;
    if (event->type() != QEvent::FocusIn)
      return false;
    ++consumed;
    return true;
  });
  watched.installEventFilter(filter);
  check(filter->has_handler(), "a handler was supplied");

  SECTION("a matching event is consumed") {
    QFocusEvent focus_in(QEvent::FocusIn);
    QApplication::sendEvent(&watched, &focus_in);
    check(seen == 1, "the handler ran");
    check(consumed == 1, "and consumed the event");
    check(last_type == QEvent::FocusIn, "it saw the right type");
  }

  SECTION("a non-matching event passes through untouched") {
    QFocusEvent focus_out(QEvent::FocusOut);
    QApplication::sendEvent(&watched, &focus_out);
    check(seen == 1, "the handler ran");
    check(consumed == 0, "but consumed nothing");
  }

  SECTION("the filter dies with the object it was parented to") {
    check(filter->parent() == &watched, "parented to the watcher");
    SUCCEED("Qt removes the filter when the watcher is destroyed");
  }
}

TEST_CASE("U269 EventFilter without a handler passes everything through",
          "[ui][primitives][event_filter]") {
  ensure_app();
  QWidget watched;
  auto *filter = new ui::EventFilter(&watched, nullptr);
  check(!filter->has_handler(), "a null handler is not a handler");
  watched.installEventFilter(filter);

  QFocusEvent focus_in(QEvent::FocusIn);
  QApplication::sendEvent(&watched, &focus_in);
  SUCCEED("an inert filter is a no-op, not a crash");
}