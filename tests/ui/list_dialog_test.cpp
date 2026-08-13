// Offscreen GUI test for the shared MO2 ListDialog port (list_dialog.{h,cpp}):
//   - setChoices populates the list in order; getChoice is empty until a row is
//     selected and follows the selection.
//   - setChoiceData rides parallel id payloads and getChoiceData round-trips
//     them even after filtering (no display-name re-resolution).
//   - The filter box narrows the list case-insensitively, auto-selects the
//     single match, draws the red border while active, and a cleared filter
//     restores every choice and the border.
//   - Double-click on an item accepts the dialog.
//   - Ok is disabled when the list is empty.
//   - The listdialog/window_geometry Settings key round-trips the geometry.
//
// Hermetic: XDG_CONFIG_HOME under /tmp; no network, no user config access.
// QT_QPA_PLATFORM=offscreen via the test property.
#include "ui/widgets/list_dialog.h"

#include "ui/settings/settings.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>
#include <filesystem>
#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char* what) {
    INFO(what);
    REQUIRE(cond);
}
}

static QListWidget* list_of(ui::ListDialog& dlg) {
    return dlg.findChild<QListWidget*>();
}

static QLineEdit* filter_of(ui::ListDialog& dlg) {
    return dlg.findChild<QLineEdit*>();
}

static QPushButton* ok_of(ui::ListDialog& dlg) {
    auto* box = dlg.findChild<QDialogButtonBox*>();
    return box ? box->button(QDialogButtonBox::Ok) : nullptr;
}

TEST_CASE("list dialog", "[ui]") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const std::filesystem::path cfg = "/tmp/gmm_list_dialog/config";
    std::filesystem::remove_all("/tmp/gmm_list_dialog");
    std::filesystem::create_directories(cfg);
    qputenv("XDG_CONFIG_HOME", cfg.c_str());
    int test_argc = 1;
    char test_argv0[] = "test";
    char* test_argv[] = {test_argv0, nullptr};
    QApplication app(test_argc, test_argv);
    QCoreApplication::setOrganizationName("GameModManager");
    QCoreApplication::setApplicationName("GameModManager");

    // ---- choices / selection ------------------------------------------------
    {
        ui::ListDialog dlg;
        dlg.setChoices({"Alpha", "Beta", "Gamma"});
        auto* list = list_of(dlg);
        check(list && list->count() == 3, "choices populate the list");
        check(list && list->item(0)->text() == "Alpha" &&
                  list->item(2)->text() == "Gamma",
              "choices keep their order");
        check(dlg.getChoice().isEmpty(), "getChoice is empty with no selection");

        if (list) {
            list->setCurrentRow(1);
            check(dlg.getChoice() == "Beta", "getChoice follows the selection");
        }
    }

    // ---- id payloads --------------------------------------------------------
    {
        ui::ListDialog dlg;
        dlg.setChoices({"SkyUI", "Unofficial Skyrim Patch"});
        dlg.setChoiceData({"sep-a", "sep-b"});
        dlg.setCurrentRow(0);
        check(dlg.getChoiceData().toString() == "sep-a",
              "getChoiceData returns the payload of the selected row");
    }

    // ---- filter -------------------------------------------------------------
    {
        ui::ListDialog dlg;
        dlg.setChoices({"SkyUI", "Unofficial Skyrim Patch", "Other"});
        dlg.setChoiceData({"sep-a", "sep-b", "sep-c"});
        auto* filter = filter_of(dlg);
        auto* list = list_of(dlg);

        if (filter) filter->setText("s");
        check(list && list->count() == 2, "filter narrows case-insensitively");
        check(!list->styleSheet().isEmpty(),
              "red border is drawn while a filter is active");

        if (filter) filter->setText("patch");
        check(list && list->count() == 1 &&
                  list->item(0)->text() == "Unofficial Skyrim Patch",
              "filter reduces to the single match");
        check(dlg.getChoice() == "Unofficial Skyrim Patch",
              "a single match is auto-selected");
        check(dlg.getChoiceData().toString() == "sep-b",
              "payload rides along through the filter");

        if (filter) filter->setText("");
        check(list && list->count() == 3, "cleared filter restores all choices");
        check(list->styleSheet().isEmpty(),
              "cleared filter removes the red border");
    }

    // ---- double-click accepts ----------------------------------------------
    {
        ui::ListDialog dlg;
        dlg.setChoices({"Alpha", "Beta"});
        QSignalSpy spy(&dlg, &QDialog::accepted);
        dlg.show();
        dlg.activateWindow();
        dlg.setFocus();
        QCoreApplication::processEvents();
        auto* list = list_of(dlg);
        if (list) {
            list->setFocus();
            const auto center = list->visualItemRect(list->item(1)).center();
            // Deliver the double-click event straight to the viewport (QTest's
            // mouse routing to a top-level offscreen dialog doesn't land).
            QMouseEvent press(QEvent::MouseButtonPress, center, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, center, Qt::LeftButton,
                                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &release);
            QMouseEvent dbl(QEvent::MouseButtonDblClick, center, Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &dbl);
            QCoreApplication::processEvents();
        }
        check(spy.count() == 1, "double-click accepts the dialog");
        check(dlg.result() == QDialog::Accepted, "accepted result set");
        dlg.close();
    }

    // ---- empty list guards the Ok button -----------------------------------
    {
        ui::ListDialog dlg;
        dlg.setChoices({});
        auto* ok = ok_of(dlg);
        check(ok && !ok->isEnabled(), "Ok is disabled with no choices");
    }

    // ---- geometry persistence through Settings ------------------------------
    {
        Settings::instance().set_listdialog_window_geometry(QByteArray());
        check(Settings::instance().listdialog_window_geometry().isEmpty(),
              "geometry key starts empty");
        ui::ListDialog dlg;
        const QByteArray geo = "test-geometry-blob";
        Settings::instance().set_listdialog_window_geometry(geo);
        check(Settings::instance().listdialog_window_geometry() == geo,
              "geometry round-trips through the Settings key");
    }
}
