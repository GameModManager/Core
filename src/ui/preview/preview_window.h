#pragma once

#include <QDialog>
#include <QLabel>
#include <QPixmap>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstddef>
#include <functional>
#include <vector>

#include "engine/game/registry/game_features/game_feature.h"

class QListWidget;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTextBrowser;
class QVBoxLayout;

namespace ui::preview {

class DebugImageLabel;

// Custom QSlider that snaps to common speed values (0.25x, 0.5x, 0.75x, 1x,
// 1.5x, 2x, 3x, 4x) when dragged nearby. The visual tick marks are drawn by
// SpeedTickStrip below the slider.
class SpeedSlider : public QSlider {
  Q_OBJECT
public:
  explicit SpeedSlider(QWidget *parent = nullptr);
};

// Widget drawn directly below the SpeedSlider. Shows a vertical tick "|" at
// each snap position and the speed label (0.25x, 0.5x, etc.) centered below
// it, aligned to the same valueToPixel mapping as the slider's groove.
class SpeedTickStrip : public QWidget {
  Q_OBJECT
public:
  explicit SpeedTickStrip(QWidget *parent = nullptr);

  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  int valueToPixel(int val) const;
  QString labelForValue(int val) const;
};

// Persistent floating preview window (MO2's PreviewDialog). Renders images
// (checkerboard background, zoom/fit controls) and text files, and browses
// the provider variants of a multi-provider file with previous/next buttons
// (MO2's variants stack).
class PreviewWindow : public QDialog {
  Q_OBJECT
public:
  explicit PreviewWindow(QWidget *parent = nullptr);

  void set_game_id(const std::string &id) { game_id_ = id; }

  // Show a preview for the given file. provider_paths lists the on-disk
  // copies of every provider (primary first) for variant browsing; the
  // primary file must be its first entry (or the sole path). An empty
  // provider list means single-file mode.
  void show_file(const QString &file_path,
                 const QStringList &provider_paths = {},
                 const QStringList &provider_names = {});

  // Whether the window can render a file (supported image or text format).
  [[nodiscard]] static bool supports(const QString &file_path);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private slots:
  void on_anm2_frame_timeout();

private:
  void reload();
  bool load_image(const QString &path);
  bool load_anm2(const QString &path);
  bool load_text(const QString &path);
  // Try a plugin-provided preview (v2 IPluginPreview) for the file's extension.
  // Returns true and embeds the plugin's QWidget* if a plugin claimed it.
  bool load_plugin_preview(const QString &path);
  void show_unsupported();
  void apply_zoom();
  void set_fit();
  void zoom_by(double factor);

  QLabel *name_label_ = nullptr;
  QLabel *source_label_ = nullptr;
  QLabel *zoom_label_ = nullptr;
  QStackedWidget *stack_ = nullptr;
  QWidget *image_page_ = nullptr;
  QScrollArea *scroll_ = nullptr;
  DebugImageLabel *image_label_ = nullptr;
  QTextBrowser *text_view_ = nullptr;
  QLabel *unsupported_label_ = nullptr;
  // Plugin-provided preview page (v2 IPluginPreview). Holds whatever QWidget*
  // a plugin returned for the selected file's extension.
  QWidget *plugin_page_ = nullptr;
  QVBoxLayout *plugin_layout_ = nullptr;
  QWidget *plugin_widget_ = nullptr;
  QPushButton *prev_button_ = nullptr;
  QPushButton *next_button_ = nullptr;

  QStringList paths_; // variant on-disk copies, aligned with names_
  QStringList names_; // variant labels (mod ids / display names)
  int variant_ = 0;
  double zoom_ = 1.0; // display scale, natural size = 1.0
  bool fit_ = true;   // scale to viewport instead of zoom_
  QPixmap current_pixmap_;

  // ANM2 animation state
  std::vector<QPixmap> anm2_frames_;
  std::vector<int> anm2_delays_;
  std::size_t anm2_index_ = 0;
  QTimer anm2_timer_;
  std::string game_id_;

  // ANM2 playback controls (two-column layout)
  QWidget *anm2_controls_ = nullptr;
  QListWidget *anm2_anim_list_ = nullptr;
  SpeedSlider *anm2_speed_slider_ = nullptr;
  QPushButton *anm2_play_btn_ = nullptr;
  QPushButton *anm2_step_back_ = nullptr;
  QPushButton *anm2_step_fwd_ = nullptr;
  QSlider *anm2_progress_ = nullptr;
  QLabel *anm2_info_label_ = nullptr;
  QLabel *anm2_frame_label_ = nullptr;

  // Parsed animation states from ANM2 file
  struct AnimationState {
    QString name;
    std::vector<QPixmap> frames;
    std::vector<int> delays;
    int fps = 30;
  };
  std::vector<AnimationState> anm2_states_;
  int anm2_current_state_ = 0;
  bool anm2_playing_ = false;

  // On-demand rendering support (when available, renders frames on the fly
  // with proper interpolation instead of using pre-baked frames)
  ::engine::AnimationParserFeature::RenderFrameFn anm2_render_fn_;
  int anm2_on_demand_canvas_w_ = 0;
  int anm2_on_demand_canvas_h_ = 0;
  int anm2_on_demand_fps_ = 0;
  int anm2_on_demand_frame_count_ = 0;
  float anm2_time_ = 0.0f; // float time counter for on-demand rendering

  // Per-state on-demand render data. When the user switches animation states,
  // the active render callback and canvas dimensions are swapped from here.
  struct StateRenderData {
    ::engine::AnimationParserFeature::RenderFrameFn render_frame;
    int canvas_w = 0;
    int canvas_h = 0;
    int fps = 0;
    int frame_count = 0;
  };
  std::vector<StateRenderData> anm2_state_renders_;

  // Debug bounding-box overlay (F12 toggle).
  bool debug_overlay_enabled_ = false;

  // Parse ANM2 data and set up the host controls panel (animation list, speed
  // slider, play/pause, scrubber, step buttons). Does NOT switch the stack
  // page.
  bool parse_anm2_data(const QString &path);
  // Build the ANM2 controls panel (created once, shown/hidden as needed).
  void build_anm2_controls();
  // Switch to the given animation state index and reset playback.
  void switch_anm2_state(int index);
  // Update the frame counter label and progress scrubber position.
  void update_anm2_ui();
};

} // namespace ui::preview
