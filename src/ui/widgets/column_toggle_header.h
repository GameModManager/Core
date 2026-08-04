#pragma once

#include <QHeaderView>

namespace ui {

class ColumnToggleHeaderView : public QHeaderView {
    Q_OBJECT
public:
    explicit ColumnToggleHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr);

    void set_column_labels(const QStringList& labels);

    // Per-section tooltips shown on hover, indexed by logical section
    // (column). Empty entries suppress the tooltip for that section.
    void set_section_tooltips(const QStringList& tooltips);
    [[nodiscard]] QString section_tooltip(int section) const;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuild_menu();
    QStringList labels_;
    QStringList tooltips_;
};

}  // namespace ui
