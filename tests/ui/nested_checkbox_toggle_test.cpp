// A mod nested under a separator must toggle from its checkbox click.
//
// User report (P0): a child mod under a separator ignores checkbox clicks -
// the state never changes. Top-level mods toggle fine. The cause is in the
// Name column's IndentDelegate: paint() draws the checkbox shifted right by
// depth * kIndentStep, but the inherited QStyledItemDelegate::editorEvent
// hit-tests the check rect at the UNSHIFTED cell edge, so the click on the
// visible checkbox of a nested row lands in empty gutter and the toggle
// never reaches the model.
//
// Nesting is same-kind only (mods never parent-link under separators), so
// the reported child is a mod nested under a mod inside the separator's
// band - depth 1, checkbox drawn 20px right of the cell edge.
//
// Drives a REAL ModView + REAL ModList (no proxy, no shims) and clicks the
// viewport where the checkbox is VISIBLY drawn - the shifted indicator rect,
// computed through the same style query the delegate's paint uses. The
// top-level row is the in-case control: same click, same view, known good.
#include "ui/settings/settings.h"
#include "ui/widgets/mod_list_model.h"
#include "ui/widgets/mod_table_view.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

constexpr int kIndentStep = 20;  // px per nesting level (IndentDelegate)

int row_of_id(const ui::ModList *model, const QString &id) {
  for (int r = 0; r < model->rowCount(); ++r) {
    if (model->mods().at(r).id == id)
      return r;
  }
  return -1;
}

// Center of the checkbox as it is VISIBLY drawn: the delegate's shared
// indicator rect (style metrics at the cell's content start plus the row's
// indent shift) - the same function paint() and editorEvent() use. Clicking
// anywhere else would not exercise the reported path.
QPoint visible_checkbox_center(const QTreeView *view, const QModelIndex &idx,
                               int depth) {
  const QRect indicator = ui::IndentDelegate::check_indicator_rect(
      view->style(), view->visualRect(idx), view, depth * kIndentStep);
  return indicator.center();
}

// Records the panel fills and checkbox indicator draws issued through the
// view's style, forwarding everything else unchanged. Only DIRECT draws are
// recorded: QCommonStyle forwards PE_IndicatorItemViewItemCheck to
// PE_IndicatorCheckBox internally, and a forwarded call must not count as the
// delegate drawing the native indicator (the reported bug is exactly the
// delegate asking for the item-view variant, which several themes override
// into a bare square instead of forwarding). Lets the paint test count
// background washes over the nested row's cell (double-paint guard) and check
// which primitive carries the check state, without depending on any specific
// theme's rendering.
class PaintSpyStyle : public QProxyStyle {
public:
  struct PanelFill {
    QRect rect;
    QBrush brush;
  };
  struct CheckDraw {
    QRect rect;
    QStyle::State state;
  };

  mutable QVector<PanelFill> panel_fills;
  mutable QVector<CheckDraw> check_draws;

  void reset() const {
    panel_fills.clear();
    check_draws.clear();
  }

  void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget) const override {
    if (depth_ == 0) {
      if (element == QStyle::PE_PanelItemViewItem) {
        if (const auto *vopt = qstyleoption_cast<const QStyleOptionViewItem *>(option))
          panel_fills.append({option->rect, vopt->backgroundBrush});
      } else if (element == QStyle::PE_IndicatorCheckBox) {
        check_draws.append({option->rect, option->state});
      }
    }
    depth_++;
    QProxyStyle::drawPrimitive(element, option, painter, widget);
    depth_--;
  }

private:
  mutable int depth_ = 0;
};

// Mimics the reported theme (non-Oxygen KDE): the item-view check-indicator
// probe answers with a tiny 1x1 rect instead of the real indicator metrics,
// which drew the nested checkbox as a dot at the elbow of the fold line.
class DegenerateProbeStyle : public PaintSpyStyle {
public:
  QRect subElementRect(SubElement element, const QStyleOption *option,
                       const QWidget *widget) const override {
    if (element == QStyle::SE_ItemViewItemCheckIndicator) {
      const QRect cell = option->rect;
      return QRect(cell.left(), cell.top() + cell.height() / 2, 1, 1);
    }
    return PaintSpyStyle::subElementRect(element, option, widget);
  }
};

}  // namespace

TEST_CASE("nested mod checkbox click toggles the mod", "[ui][modlist][nesting]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  // Throwaway config root: ModList reads Settings (separator icon keys).
  const fs::path root = "/tmp/gmm_nested_checkbox_toggle";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "nested_checkbox_toggle_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  Settings::instance().set_nxm_handler_check("dont_ask");

  ui::ModList model;
  model.set_nesting_enabled(true);

  // Rows: a top-level mod, a separator, a mod in its band, and that mod's
  // nested child (the reported row).
  QVector<ui::ModEntry> entries;
  ui::ModEntry top;
  top.id      = QStringLiteral("TopMod");
  top.name    = top.id;
  top.enabled = true;
  entries.append(top);
  ui::ModEntry sep;
  sep.id           = QStringLiteral("Pack");
  sep.name         = sep.id;
  sep.enabled      = true;
  sep.is_separator = true;
  entries.append(sep);
  ui::ModEntry parent;
  parent.id      = QStringLiteral("CrashLogger");
  parent.name    = parent.id;
  parent.enabled = true;
  entries.append(parent);
  ui::ModEntry child;
  child.id        = QStringLiteral("CrashLogger SSE PDB");
  child.name      = child.id;
  child.enabled   = true;
  child.parent_id = parent.id;
  entries.append(child);
  model.reset_with_order(entries);

  ui::ModView view;
  view.setModel(&model);
  view.resize(600, 300);
  view.show();
  QApplication::processEvents();

  const int top_row   = row_of_id(&model, QStringLiteral("TopMod"));
  const int child_row = row_of_id(&model, QStringLiteral("CrashLogger SSE PDB"));
  REQUIRE(top_row >= 0);
  REQUIRE(child_row >= 0);
  REQUIRE(model.mods()[child_row].parent_id == QStringLiteral("CrashLogger"));
  REQUIRE(model.nesting_depth(child_row) == 1);
  REQUIRE(model.mods()[child_row].enabled);

  const QModelIndex child_idx = model.index(child_row, ui::ModList::Name);
  const QPoint child_click =
      visible_checkbox_center(&view, child_idx, model.nesting_depth(child_row));
  // The click must land on the drawn checkbox, not somewhere else.
  REQUIRE(view.visualRect(child_idx).contains(child_click));

  // THE POINT: clicking the nested row's visible checkbox flips the state.
  QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, child_click);
  QApplication::processEvents();
  CHECK_FALSE(model.mods()[child_row].enabled);

  // Control: the same click on the top-level row (depth 0, unshifted) still
  // toggles - the fix must not break the working path.
  const QModelIndex top_idx = model.index(top_row, ui::ModList::Name);
  QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                    visible_checkbox_center(&view, top_idx, 0));
  QApplication::processEvents();
  CHECK_FALSE(model.mods()[top_row].enabled);

  // And both toggle back on.
  QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, child_click);
  QApplication::processEvents();
  CHECK(model.mods()[child_row].enabled);
  QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                    visible_checkbox_center(&view, top_idx, 0));
  QApplication::processEvents();
  CHECK(model.mods()[top_row].enabled);

  fs::remove_all(root, ec);
}

// Follow-up user report on the same nested rows: the Name cell painted the
// checkbox as a custom square (the item-view check primitive, which several
// themes render as a bare box - empty on enabled rows) and washed the cell
// background twice (full-width pass + content pass over alpha-blended
// conflict/highlight tints = the doubled band). The checkbox must be the
// theme's native PE_IndicatorCheckBox carrying the real CheckStateRole state,
// and the background must be painted exactly once.
TEST_CASE("nested row paints the native checkbox once", "[ui][modlist][nesting]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = "/tmp/gmm_nested_checkbox_paint";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "nested_checkbox_toggle_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  Settings::instance().set_nxm_handler_check("dont_ask");

  ui::ModList model;
  model.set_nesting_enabled(true);

  // Top-level rows DISABLED, nested child ENABLED: the only State_On checkbox
  // draw in the render must be the child's.
  QVector<ui::ModEntry> entries;
  ui::ModEntry top;
  top.id      = QStringLiteral("TopMod");
  top.name    = top.id;
  top.enabled = false;
  entries.append(top);
  ui::ModEntry sep;
  sep.id           = QStringLiteral("Pack");
  sep.name         = sep.id;
  sep.enabled      = false;
  sep.is_separator = true;
  entries.append(sep);
  ui::ModEntry parent;
  parent.id      = QStringLiteral("CrashLogger");
  parent.name    = parent.id;
  parent.enabled = false;
  entries.append(parent);
  ui::ModEntry child;
  child.id        = QStringLiteral("CrashLogger SSE PDB");
  child.name      = child.id;
  child.enabled   = true;
  child.parent_id = parent.id;
  entries.append(child);
  model.reset_with_order(entries);

  ui::ModView view;
  view.setModel(&model);
  PaintSpyStyle spy;
  view.setStyle(&spy);
  view.resize(600, 300);
  view.show();
  QApplication::processEvents();

  const int child_row = row_of_id(&model, QStringLiteral("CrashLogger SSE PDB"));
  REQUIRE(child_row >= 0);
  const QModelIndex child_idx = model.index(child_row, ui::ModList::Name);
  const QRect child_cell      = view.visualRect(child_idx);
  REQUIRE(child_cell.isValid());

  // Semi-transparent row tint on the nested row (the plugin-selected
  // highlight, alpha 64 by default): a background painted twice doubles the
  // wash visibly - the reported doubled band.
  model.set_highlighted_mods({QStringLiteral("CrashLogger SSE PDB")});

  QImage image(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
  const auto render = [&view, &image, &spy] {
    spy.reset();
    image.fill(Qt::transparent);
    view.viewport()->render(&image);
  };

  render();

  // Tinted panel fills over the nested cell: the gutter pass and the content
  // pass must be disjoint, so every pixel of the cell is washed exactly once.
  QVector<QRect> tinted;
  for (const auto &fill : spy.panel_fills)
    if (fill.brush.style() != Qt::NoBrush && fill.brush.color().alpha() > 0 &&
        fill.rect.intersects(child_cell))
      tinted.append(fill.rect);
  REQUIRE(tinted.size() >= 1);
  for (int i = 0; i < tinted.size(); ++i)
    for (int j = i + 1; j < tinted.size(); ++j)
      CHECK_FALSE(tinted[i].intersects(tinted[j]));

  // The nested row's checkbox is the theme's native indicator carrying the
  // real state: the enabled row draws PE_IndicatorCheckBox with State_On.
  bool on_in_child = false;
  for (const auto &draw : spy.check_draws)
    if (draw.state.testFlag(QStyle::State_On) &&
        child_cell.contains(draw.rect.center()))
      on_in_child = true;
  CHECK(on_in_child);

  // And the indicator follows the state when the row is disabled.
  REQUIRE(model.setData(child_idx, Qt::Unchecked, Qt::CheckStateRole));
  QApplication::processEvents();
  render();
  bool off_in_child = false;
  for (const auto &draw : spy.check_draws) {
    if (!child_cell.contains(draw.rect.center()))
      continue;
    CHECK_FALSE(draw.state.testFlag(QStyle::State_On));
    if (draw.state.testFlag(QStyle::State_Off))
      off_in_child = true;
  }
  CHECK(off_in_child);

  fs::remove_all(root, ec);
}

// Follow-up user report on the same nested rows, after the native-checkbox
// fix: on the user's theme (non-Oxygen KDE) the nested rows show NO checkbox
// at all, just a tiny dot at the elbow of the fold line. The parent (depth 0)
// checkbox is fine. Cause: paint() and editorEvent() both positioned the
// indicator through subElementRect(SE_ItemViewItemCheckIndicator), and that
// probe answers a tiny/empty rect on several themes (KDE especially) - the
// drawn checkbox shrank to a dot. The fix sizes the indicator from the
// style's own metrics (PM_IndicatorWidth / PM_IndicatorHeight) at the cell's
// content start plus the indent shift, one shared rect for paint and
// hit-test, and never consults the probe. This test simulates the reported
// theme with a degenerate probe and requires both the drawn checkbox and the
// click on its painted pixels to survive it.
TEST_CASE("nested checkbox survives a degenerate indicator probe",
          "[ui][modlist][nesting]") {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  const fs::path root = "/tmp/gmm_nested_checkbox_degenerate";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "config");
  qputenv("XDG_CONFIG_HOME", QByteArray((root / "config").string().c_str()));

  int test_argc     = 1;
  char test_argv0[] = "nested_checkbox_toggle_test";
  char *test_argv[] = {test_argv0, nullptr};
  QApplication app(test_argc, test_argv);
  QCoreApplication::setOrganizationName("GameModManager");
  QCoreApplication::setApplicationName("GameModManager");
  Settings::instance().set_nxm_handler_check("dont_ask");

  ui::ModList model;
  model.set_nesting_enabled(true);

  // Top-level rows DISABLED, nested child ENABLED: the only State_On checkbox
  // draw in the render must be the child's.
  QVector<ui::ModEntry> entries;
  ui::ModEntry top;
  top.id      = QStringLiteral("TopMod");
  top.name    = top.id;
  top.enabled = false;
  entries.append(top);
  ui::ModEntry sep;
  sep.id           = QStringLiteral("Pack");
  sep.name         = sep.id;
  sep.enabled      = false;
  sep.is_separator = true;
  entries.append(sep);
  ui::ModEntry parent;
  parent.id      = QStringLiteral("CrashLogger");
  parent.name    = parent.id;
  parent.enabled = false;
  entries.append(parent);
  ui::ModEntry child;
  child.id        = QStringLiteral("CrashLogger SSE PDB");
  child.name      = child.id;
  child.enabled   = true;
  child.parent_id = parent.id;
  entries.append(child);
  model.reset_with_order(entries);

  ui::ModView view;
  view.setModel(&model);
  DegenerateProbeStyle spy;
  view.setStyle(&spy);
  view.resize(600, 300);
  view.show();
  QApplication::processEvents();

  const int child_row = row_of_id(&model, QStringLiteral("CrashLogger SSE PDB"));
  REQUIRE(child_row >= 0);
  REQUIRE(model.nesting_depth(child_row) == 1);
  const QModelIndex child_idx = model.index(child_row, ui::ModList::Name);
  const QRect child_cell      = view.visualRect(child_idx);
  REQUIRE(child_cell.isValid());

  QImage image(view.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
  spy.reset();
  image.fill(Qt::transparent);
  view.viewport()->render(&image);

  // The drawn indicator must be non-degenerate: at least the style's own
  // indicator metrics. The dot regression drew it from the subElementRect
  // probe, which the simulated theme answers with a 1x1 rect.
  const int min_w =
      view.style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, &view);
  const int min_h =
      view.style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, &view);
  REQUIRE(min_w > 1);
  bool on_in_child = false;
  for (const auto &draw : spy.check_draws) {
    if (!child_cell.contains(draw.rect.center()))
      continue;
    if (draw.state.testFlag(QStyle::State_On)) {
      on_in_child = true;
      CHECK(draw.rect.width() >= min_w);
      CHECK(draw.rect.height() >= min_h);
    }
  }
  REQUIRE(on_in_child);

  // And the click path must hit the painted pixels: clicking the center of
  // the recorded State_On indicator rect toggles the mod, so the drawn
  // checkbox and the clickable area can never diverge.
  QPoint drawn_center;
  bool have_drawn = false;
  for (const auto &draw : spy.check_draws) {
    if (child_cell.contains(draw.rect.center()) &&
        draw.state.testFlag(QStyle::State_On)) {
      drawn_center = draw.rect.center();
      have_drawn   = true;
    }
  }
  REQUIRE(have_drawn);
  QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, drawn_center);
  QApplication::processEvents();
  CHECK_FALSE(model.mods()[child_row].enabled);

  fs::remove_all(root, ec);
}
