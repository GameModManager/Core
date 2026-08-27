#pragma once

#include <QDialog>
#include <QLabel>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstddef>
#include <vector>

class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTextBrowser;
class QVBoxLayout;

namespace ui::preview {

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
  QLabel *image_label_ = nullptr;
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
};

} // namespace ui::preview
