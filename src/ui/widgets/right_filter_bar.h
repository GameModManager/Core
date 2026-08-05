#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QTableWidget;

namespace ui {

// Persistent filter bar below the right panel's tab widget.
// Survives tab switches - the same text filters whichever tab is active.
class RightFilterBar : public QWidget {
    Q_OBJECT
public:
    explicit RightFilterBar(QWidget* parent = nullptr);

    [[nodiscard]] QString filter_text() const;

    // Apply the current filter text to the given table
    void apply_to(QTableWidget* table) const;

    // Show/hide the LOOT sort shortcut. Only meaningful on the Plugins tab.
    void set_sort_visible(bool visible);

signals:
    void filter_changed(const QString& text);
    void sort_requested();

private:
    QLineEdit* filter_edit_ = nullptr;
    QPushButton* sort_button_ = nullptr;
};

}  // namespace ui
