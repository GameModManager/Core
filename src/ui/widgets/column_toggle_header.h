#pragma once

#include <QHeaderView>

namespace ui {

class ColumnToggleHeaderView : public QHeaderView {
  Q_OBJECT
public:
  explicit ColumnToggleHeaderView(Qt::Orientation orientation,
                                  QWidget *parent = nullptr);

  // The labels the context menu was given, in section order. The list is
  // positional: a caller that passes fewer entries than the view has sections
  // leaves the remainder rendering as "Column N" (see eventFilter).
  void set_column_labels(const QStringList &labels);
  [[nodiscard]] QStringList column_labels() const { return labels_; }

  // True once the user has changed a section's visibility from the menu. A
  // saved QHeaderView state records visibility but not intent, so a caller
  // that re-applies a default-hidden set after restoring a saved state reads
  // this to leave an explicit choice alone.
  void note_user_visibility_choice();
  [[nodiscard]] bool has_user_visibility_choice() const {
    return user_visibility_choice_;
  }

  // Lock a section so it can never be hidden: the context menu entry is
  // shown checked + disabled, and a toggled hide for it is refused. Multiple
  // sections may be locked (e.g. the mod list's Name column).
  void set_locked_section(int section);
  void set_locked_sections(const QList<int> &sections);
  [[nodiscard]] bool is_locked(int section) const;

  // Per-section tooltips shown on hover, indexed by logical section
  // (column). Empty entries suppress the tooltip for that section.
  void set_section_tooltips(const QStringList &tooltips);
  [[nodiscard]] QString section_tooltip(int section) const;

signals:
  // Emitted ONLY from a user toggle in the context menu (never for
  // programmatic showSection/hideSection), so callers can persist the
  // user's choice without re-saving their own restores.
  void section_toggled(int logical, bool hidden);

protected:
  bool eventFilter(QObject *obj, QEvent *event) override;

private:
  QStringList labels_;
  QStringList tooltips_;
  QList<int> locked_sections_;
  bool user_visibility_choice_ = false;
};

}  // namespace ui
