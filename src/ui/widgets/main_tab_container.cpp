#include "ui/widgets/main_tab_container.h"

#include <QTabBar>

#include "ui/settings/settings.h"

namespace ui {

MainTabContainer::MainTabContainer(QWidget* parent) : QTabWidget(parent) {
    // Dynamic tabs get a close button; the Main tab's button is removed in
    // add_main_tab() so it can never be closed.
    setTabsClosable(true);
    // Flat, document-style tab bar (no page border) - matches the modern
    // look of the right-panel tabs and keeps the page frameless like the
    // pre-tab central widget.
    setDocumentMode(true);
    // Hidden by default: with Full UI mode OFF the window must look exactly
    // like the pre-tab layout.
    tabBar()->setVisible(false);

    connect(this, &QTabWidget::tabCloseRequested, this,
            &MainTabContainer::on_tab_close_requested);
}

void MainTabContainer::add_main_tab(QWidget* content) {
    if (!content || count() > 0)
        return;
    addTab(content, tr("Main"));
    // The Main tab is permanent: no close button, always at index 0.
    tabBar()->setTabButton(kMainTabIndex, QTabBar::RightSide, nullptr);
    update_tab_bar_visibility();
}

int MainTabContainer::add_view_tab(QWidget* content, const QString& title,
                                   const QString& key) {
    if (!content)
        return -1;
    if (!key.isEmpty() && has_tab(key)) {
        select_tab(key);
        return indexOf(tab_keys_.value(key));
    }
    const int index = addTab(content, title);
    if (!key.isEmpty())
        tab_keys_.insert(key, content);
    setCurrentIndex(index);
    update_tab_bar_visibility();
    return index;
}

void MainTabContainer::remove_view_tab(int index) {
    if (index <= kMainTabIndex || index >= count())
        return;  // never remove the Main tab
    QWidget* page = widget(index);
    forget_key_for(page);
    removeTab(index);  // does NOT delete the page widget
    emit view_tab_removed(page);
    update_tab_bar_visibility();
}

void MainTabContainer::remove_view_tab(const QString& key) {
    if (key.isEmpty())
        return;
    auto it = tab_keys_.find(key);
    if (it == tab_keys_.end())
        return;
    QWidget* page = it.value();
    tab_keys_.erase(it);
    const int index = indexOf(page);
    if (index > kMainTabIndex) {
        removeTab(index);  // does NOT delete the page widget
        emit view_tab_removed(page);
    }
    update_tab_bar_visibility();
}

void MainTabContainer::close_all_view_tabs() {
    // Remove from the end so the remaining indexes stay valid.
    for (int i = count() - 1; i > kMainTabIndex; --i) {
        QWidget* page = widget(i);
        forget_key_for(page);
        removeTab(i);
        emit view_tab_removed(page);
    }
    update_tab_bar_visibility();
}

bool MainTabContainer::has_tab(const QString& key) const {
    if (key.isEmpty())
        return false;
    auto it = tab_keys_.constFind(key);
    return it != tab_keys_.constEnd() && it.value() && indexOf(it.value()) >= 0;
}

void MainTabContainer::select_tab(const QString& key) {
    if (key.isEmpty())
        return;
    auto it = tab_keys_.constFind(key);
    if (it == tab_keys_.constEnd() || !it.value())
        return;
    setCurrentWidget(it.value());
}

void MainTabContainer::update_tab_bar_visibility() {
    const bool visible = Settings::instance().full_ui_mode() && count() > 1;
    tabBar()->setVisible(visible);
}

void MainTabContainer::on_tab_close_requested(int index) {
    if (index <= kMainTabIndex)
        return;  // the Main tab is not closable
    remove_view_tab(index);
}

void MainTabContainer::forget_key_for(QWidget* page) {
    for (auto it = tab_keys_.begin(); it != tab_keys_.end();) {
        if (it.value() == page)
            it = tab_keys_.erase(it);
        else
            ++it;
    }
}

}  // namespace ui