#pragma once

#include "ui/modinfo/description_renderer.h"

#include <QWebEnginePage>
#include <QWebEngineView>

namespace ui
{

// QWebEngineView backend (the default). A QWidget, so it embeds directly in
// the layout - no createWindowContainer needed. Wraps fragments in an
// MO2-style dark shell, disables JavaScript (descriptions are untrusted
// remote content), and turns link clicks into link_clicked via a
// QWebEnginePage intercepting acceptNavigationRequest.
class WebViewDescriptionRenderer : public DescriptionRenderer
{
  Q_OBJECT
public:
  explicit WebViewDescriptionRenderer(QWidget* parent = nullptr);

  void set_description(const QString& html) override;
  void clear() override;
  void set_source_style(SourceCSS style) override { current_style_ = style; }
  [[nodiscard]] QString current_description() const override
  {
    return current_html_;
  }

private:
  // Page subclass intercepting link clicks: external links are re-emitted as
  // link_clicked for the panel's open_url path, and in-place navigation is
  // always refused so the view never leaves our content. No Q_OBJECT: moc
  // does not support nested classes, and none is needed (no signals/slots;
  // the override is plain virtual and the nested class may emit the outer
  // class's signal through normal member access).
  class InterceptPage : public QWebEnginePage
  {
  public:
    using QWebEnginePage::QWebEnginePage;

  protected:
    bool acceptNavigationRequest(
        const QUrl& url, NavigationType type,
        bool isMainFrame) override;
  };

  QWebEngineView* webview_ = nullptr;
  QString current_html_;
  SourceCSS current_style_ = SourceCSS::Default;
};

}  // namespace ui
