#pragma once

#include <QWidget>

class QTableWidget;

namespace ui {

class PluginsTab : public QWidget {
    Q_OBJECT
public:
    explicit PluginsTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class ArchivesTab : public QWidget {
    Q_OBJECT
public:
    explicit ArchivesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class DataTab : public QWidget {
    Q_OBJECT
public:
    explicit DataTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class SavesTab : public QWidget {
    Q_OBJECT
public:
    explicit SavesTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

class DownloadsTab : public QWidget {
    Q_OBJECT
public:
    explicit DownloadsTab(QWidget* parent = nullptr);
    [[nodiscard]] QTableWidget* table() const { return table_; }
private:
    QTableWidget* table_ = nullptr;
};

}  // namespace ui
