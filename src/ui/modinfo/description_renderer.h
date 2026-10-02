#pragma once

#include <QUrl>
#include <QWidget>

namespace ui {

class DescriptionBrowser;

// Per-source CSS theme for the WebEngine backend. Each source panel
// selects its own theme so descriptions render in the look of the site
// they came from (Nexus keeps the MO2-style dark shell, LoversLab gets
// the Invision Community dark theme). The TextBrowser fallback ignores
// the value (no CSS theming there).
enum class SourceCSS { Default, Nexus, LoversLab, Steam, ModPub };

// Keeps the description view away from a faulting Vulkan path.
//
// With a Vulkan overlay interposed - MangoHud does exactly this whenever
// MANGOHUD=1 is in the environment, and LSFG installs a layer the loader then
// refuses - the first vkCreateDevice reaches the overlay's shim, which
// dispatches through a callback it never installed, and the process jumps to
// address 0. That is a fault inside a third-party library in this process, so
// the only thing available is to keep the call from being made: Chromium is
// told to skip the GPU process, and the Vulkan loader is pointed at no driver
// so nothing can interpose on it at all. See the .cpp for what was measured.
//
// Runs before any Qt object exists, from a static initialiser in the .cpp, so
// it cannot be forgotten by a new entry point or skipped in a test binary.
// Declared here rather than next to the WebEngine backend because this header
// is the only one every build of the app and its tests can include.
void configure_chromium_flags();

// Description view shared by the four source panels (Nexus / Steam /
// LoversLab / mod.pub). Pure interface so the panels don't care which
// backend renders: the QWebEngineView implementation (default, web-faithful)
// or the QTextBrowser fallback (no WebEngineWidgets module at build time).
//
// Contract:
//   - set_description() takes an HTML fragment: usually bbcode_to_html()
//     output (Nexus / Steam / mod.pub), or raw site HTML for sources
//     whose panel bypasses the BBCode pipeline (LoversLab). Each backend
//     applies its own shell (per-source dark CSS for WebEngine, pre-wrap
//     for text).
//   - clear() drops the current content AND any in-flight async work, so a
//     stale parse can never paint into the next mod's view.
//   - Link clicks never navigate in place: backends emit link_clicked and
//     the panel opens the URL externally (data_.open_url).
class DescriptionRenderer : public QWidget {
  Q_OBJECT
public:
  explicit DescriptionRenderer(QWidget *parent = nullptr) : QWidget(parent) {}
  ~DescriptionRenderer() override = default;

  virtual void set_description(const QString &html) = 0;
  virtual void clear()                              = 0;
  // Selects the per-source CSS theme (WebEngine backend only; the
  // TextBrowser fallback stores the value and ignores it).
  virtual void set_source_style(SourceCSS style) = 0;
  // Last fragment handed to set_description() (unwrapped). Lets tests and
  // future re-renders read back what is shown without scraping the view.
  [[nodiscard]] virtual QString current_description() const = 0;

signals:
  void link_clicked(const QUrl &url);
};

// Factory: WebEngine backend when built with GMM_HAS_WEBENGINE, QTextBrowser
// fallback otherwise. The default is the WebEngine renderer.
DescriptionRenderer *create_description_renderer(QWidget *parent = nullptr);

// QTextBrowser fallback: a DescriptionRenderer owning a DescriptionBrowser
// child. Links open externally via link_clicked (same contract as the
// WebEngine backend); images keep the async path DescriptionBrowser has.
class TextBrowserDescriptionRenderer : public DescriptionRenderer {
  Q_OBJECT
public:
  explicit TextBrowserDescriptionRenderer(QWidget *parent = nullptr);

  void set_description(const QString &html) override;
  void clear() override;
  void set_source_style(SourceCSS style) override { style_ = style; }
  [[nodiscard]] QString current_description() const override { return current_; }

private:
  DescriptionBrowser *browser_ = nullptr;
  QString current_;
  SourceCSS style_ = SourceCSS::Default;
};

}  // namespace ui
