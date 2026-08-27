#pragma once

#include <memory>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QPoint>
#include <filesystem>



namespace ui::preview {

// Shared checkerboard tile for transparency preview backgrounds. mode is
// "checker_light", "checker_dark" or "auto" (detects the current palette).
// Used by both the hover PreviewWidget and the persistent PreviewWindow.
[[nodiscard]] QPixmap checker_pixmap(const QString& mode);

// Floating tooltip-style popup for previewing .png images and .anm2 animations.
//
// Shows near the cursor, auto-hides after the mouse moves away.
// Supports: static PNG display, animated .anm2 playback with configurable FPS,
// checkerboard background for transparency, context menu for animation toggle.
class PreviewWidget : public QLabel {
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;

    // Show preview for a file path at the given global position.
    // Returns false if the file type isn't supported.
    bool show_preview(const QString& file_path, const QPoint& global_pos,
                      bool debounce = true);

    // Stop any running preview and hide.
    void stop();

    // Settings
    void set_animate_anm2(bool animate);
    [[nodiscard]] bool animate_anm2() const { return animate_anm2_; }

    void set_background_mode(const QString& mode); // "auto", "checker_dark", "checker_light", "solid"
    void set_background_color(const QString& color);
    void set_border_color(const QString& color);

    // Game id used to resolve the animation parser from the registry. Mirrors
    // PreviewWindow::set_game_id(): when empty, the global (non-game-specific)
    // parser is used via the registry's wildcard fallback.
    void set_game_id(const std::string& id) { game_id_ = id; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
    void on_frame_timeout();
    void on_debounce_fire();

private:
    void apply_style();
    QPixmap make_checker(const QString& c1, const QString& c2);
    QPixmap get_checker_pixmap();

    // Try to load and display a file
    bool try_load_png(const QString& path);
    bool try_load_anm2(const QString& path);

    bool animate_anm2_ = true;
    QString bg_mode_ = "auto";
    QString bg_color_;
    QString border_color_;

    // ANM2 animation state

    std::vector<QPixmap> anm2_frames_;
    std::vector<int> anm2_delays_;
    size_t anm2_index_ = 0;
    QTimer anm2_timer_;

    // Debounce
    QTimer debounce_timer_;
    QString pending_path_;
    QPoint pending_pos_;

    // Game id used to resolve the animation parser (empty = global/wildcard).
    std::string game_id_;

    // Static checker cache - lazy-initialized in get_checker_pixmap()
    // (not static members: QPixmap requires QApplication at construction time)
};

}  // namespace ui::preview
