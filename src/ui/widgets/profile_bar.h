#pragma once

#include <QWidget>

class QComboBox;
class QToolButton;
class QHBoxLayout;

namespace ui {

class ProfileBar : public QWidget {
    Q_OBJECT
public:
    explicit ProfileBar(QWidget* parent = nullptr);

signals:
    void profile_changed(const QString& profile);
    void import_clicked();
    void export_clicked();
    void export_modlist_clicked();
    void import_modlist_clicked();
    void create_separator_clicked();
    void create_empty_mod_clicked();

private:
    QComboBox* profile_combo_ = nullptr;
    QToolButton* import_btn_ = nullptr;
    QToolButton* export_btn_ = nullptr;
    QToolButton* import_export_btn_ = nullptr;
    QToolButton* create_btn_ = nullptr;
};

}  // namespace ui
