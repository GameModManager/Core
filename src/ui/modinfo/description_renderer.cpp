#include "ui/modinfo/description_renderer.h"

#include "ui/modinfo/description_browser.h"
#ifdef GMM_HAS_WEBENGINE
#include "ui/modinfo/webview_description_renderer.h"
#endif

#include <QDebug>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace ui
{

namespace
{

  // QTextBrowser shell: the BBCode pipeline emits raw '\n' newlines (libcbb
  // preserves them), so the body needs white-space:pre-wrap to show line
  // breaks. Same wrapper the panels used before the renderer split.
  QString wrap_text_html(const QString& body)
  {
    return QStringLiteral("<html><body style=\"font-family:sans-serif; "
                          "white-space:pre-wrap;\">"
                          "%1</body></html>")
        .arg(body);
  }

}  // namespace

DescriptionRenderer* create_description_renderer(QWidget* parent)
{
#ifdef GMM_HAS_WEBENGINE
  qDebug() << "[DescRenderer] Creating renderer, GMM_HAS_WEBENGINE defined: yes";
  return new WebViewDescriptionRenderer(parent);
#else
  qDebug() << "[DescRenderer] Creating renderer, GMM_HAS_WEBENGINE defined: no, "
              "using QTextBrowser fallback";
  return new TextBrowserDescriptionRenderer(parent);
#endif
}

TextBrowserDescriptionRenderer::TextBrowserDescriptionRenderer(QWidget* parent)
    : DescriptionRenderer(parent), browser_(new DescriptionBrowser(this))
{
  // Links open externally via link_clicked so both backends share the
  // panel's data_.open_url path (matches the old setOpenExternalLinks(true)
  // behavior for the user).
  browser_->setOpenExternalLinks(false);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(browser_);
  connect(browser_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
    emit link_clicked(url);
  });
}

void TextBrowserDescriptionRenderer::set_description(const QString& html)
{
  current_ = html;
  browser_->setHtml(wrap_text_html(html));
}

void TextBrowserDescriptionRenderer::clear()
{
  current_.clear();
  browser_->clear_image_cache();
  browser_->clear();
}

}  // namespace ui
