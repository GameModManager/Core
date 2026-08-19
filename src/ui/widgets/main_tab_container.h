#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QTabWidget>

namespace ui {

// Top-level tab host for Full-UI Tab Mode. Replaces the plain central widget:
// tab 0 is the permanent "Main" tab holding the console splitter (left panel
// + right panel); tabs 1+ are dynamic view tabs (Settings, Pipeline, ...)
// opened by TabModeController.
//
// The tab bar is hidden by default and only becomes visible when Full UI
// mode is ON and at least one dynamic tab is open, so with the mode OFF the
// window looks exactly like the pre-tab layout.
class MainTabContainer : public QTabWidget {
    Q_OBJECT
public:
    explicit MainTabContainer(QWidget* parent = nullptr);

    // Adds the permanent "Main" tab wrapping `content` (the console
    // splitter). Must be called once, before any view tab. The Main tab is
    // never closable and always sits at index 0.
    void add_main_tab(QWidget* content);

    // Adds a dynamic view tab titled `title`. When `key` is non-empty the
    // tab is registered under it: opening a tab with an already-open key
    // selects the existing tab instead of duplicating it. Returns the tab
    // index (the existing one when the tab was already open).
    int add_view_tab(QWidget* content, const QString& title,
                     const QString& key = QString());

    // Removes the dynamic tab at `index` (no-op for the Main tab). The page
    // widget is NOT deleted (QTabWidget::removeTab semantics) - callers keep
    // ownership.
    void remove_view_tab(int index);

    // Removes the dynamic tab registered under `key` (no-op when absent).
    void remove_view_tab(const QString& key);

    // Closes every dynamic tab, leaving only the Main tab.
    void close_all_view_tabs();

    // True when a dynamic tab registered under `key` is open.
    [[nodiscard]] bool has_tab(const QString& key) const;

    // Selects the tab registered under `key` (no-op when absent).
    void select_tab(const QString& key);

    // Shows the tab bar only when Full UI mode is ON and more than one tab
    // exists (Main + at least one dynamic tab). Hidden otherwise.
    void update_tab_bar_visibility();

    // Index of the permanent Main tab.
    static constexpr int kMainTabIndex = 0;

signals:
    // Emitted after a dynamic tab is removed (close button, remove_view_tab
    // or close_all_view_tabs). `page` is the removed page widget, still alive
    // (QTabWidget::removeTab does not delete it) - the receiver decides
    // whether to keep or release it.
    void view_tab_removed(QWidget* page);

private:
    void on_tab_close_requested(int index);
    void forget_key_for(QWidget* page);

    // key -> page widget for dynamic tabs (untracked tabs have no entry).
    QHash<QString, QPointer<QWidget>> tab_keys_;
};

}  // namespace ui