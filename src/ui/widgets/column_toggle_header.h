#pragma once

#include <QHeaderView>

namespace ui {

class ColumnToggleHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit ColumnToggleHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr);

    void set_column_labels(const QStringList& labels);

    // Lock a section so it can never be hidden: the context menu entry is
    // shown checked + disabled, and a toggled hide for it is refused. Multiple
    // sections may be locked (e.g. the mod list's Name column).
    void set_locked_section(int section);
    void set_locked_sections(const QList<int>& sections);
    [[nodiscard]] bool is_locked(int section) const;

    // Per-section tooltips shown on hover, indexed by logical section
    // (column). Empty entries suppress the tooltip for that section.
    void set_section_tooltips(const QStringList& tooltips);
    [[nodiscard]] QString section_tooltip(int section) const;

signals:
    // Emitted ONLY from a user toggle in the context menu (never for
    // programmatic showSection/hideSection), so callers can persist the
    // user's choice without re-saving their own restores.
    void section_toggled(int logical, bool hidden);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QStringList labels_;
    QStringList tooltips_;
    QList<int> locked_sections_;
};

}  // namespace ui
