#include "ui/modinfo/webview_description_renderer.h"

#include <QDesktopServices>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineSettings>

namespace ui
{

namespace
{

// Default page background per source theme. QWebEngine paints its own
// default (white) until our setHtml CSS lands, so the page background must
// be dark or every content swap flashes white regardless of the HTML shell.
QColor page_background(SourceCSS style)
{
  if (style == SourceCSS::LoversLab)
    return QColor(0x2a, 0x2a, 0x2e);
  return QColor(0x40, 0x40, 0x40);
}

  // MO2-style dark shell around BBCode-generated fragments (Nexus /
  // Steam / mod.pub, plus the Default style). white-space:pre-wrap stays
  // because the pipeline emits raw '\n' newlines (libcbb preserves them);
  // without it multi-line descriptions would collapse into one paragraph
  // in the webview.
  //
  // LoversLab fragments are raw Invision Community HTML (block tags
  // intact), so they get the Invision dark theme instead: no pre-wrap
  // (real <p>/<br> carry the breaks), LL background/text/link colors.
  QString wrap_web_html(const QString& body, SourceCSS style)
  {
    static const QString kNexusCss = QStringLiteral(
        "body{font-family:sans-serif;font-size:14px;background:#404040;color:"
        "#f1f1f1;max-width:1060px;margin:auto;padding:20px "
        "7px;white-space:pre-wrap}"
        "img{max-width:100%}"
        "figure.quote{border-left:6px solid #57a5cc;background:#383838;padding:"
        "10px 15px;margin:10px 0}"
        "div.spoiler_content{background:#262626;border:1px dashed "
        "#3b3b3b;padding:10px}"
        "a{overflow-wrap:break-word;color:#8197ec;text-decoration:none}"
        "details summary::marker{display:none}");
    // Invision Community dark theme (colors sampled from the live LL
    // dark style). Static, hardcoded - no runtime fetching.
    static const QString kLoversLabCss = QStringLiteral(
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe "
        "UI',Roboto,sans-serif;font-size:14px;background:#2a2a2e;color:"
        "#c5c8cb;max-width:1060px;margin:auto;padding:20px 7px;line-"
        "height:1.6}"
        "img{max-width:100%;height:auto}"
        "a{color:#708fe2;text-decoration:none}"
        "a:hover{text-decoration:underline}"
        "ul,ol{margin:8px 0;padding-left:24px}"
        "li{margin:4px 0}"
        "p{margin:8px 0}"
        "strong,b{font-weight:600}"
        "em,i{font-style:italic}"
        "code{background:#1e1e22;padding:2px 6px;border-radius:3px;font-"
        "family:'SF Mono',Monaco,Consolas,monospace;font-size:13px}"
        "pre{background:#1e1e22;padding:12px;border-radius:4px;overflow-x:auto}"
        "pre code{background:none;padding:0}"
        "blockquote,figure.quote{border-left:4px solid #708fe2;background:#1e1e22;"
        "padding:8px 16px;margin:12px 0}"
        "hr{border:none;border-top:1px solid #444;margin:16px 0}"
        "table{border-collapse:collapse;width:100%}"
        "th,td{border:1px solid #444;padding:6px 12px;text-align:left}"
        "th{background:#1e1e22;font-weight:600}");
    const QString css =
        (style == SourceCSS::LoversLab) ? kLoversLabCss : kNexusCss;
    return QStringLiteral("<html><head><style>%1</style></head><body>%2</body></html>")
        .arg(css, body);
  }

}  // namespace

bool WebViewDescriptionRenderer::InterceptPage::acceptNavigationRequest(
    const QUrl& url, NavigationType /*type*/, bool isMainFrame)
{
  if (!isMainFrame)
    return false;
  // Our own setHtml() content is a data: URL - let it load.
  if (url.scheme().compare(QStringLiteral("data"), Qt::CaseInsensitive) == 0)
    return true;
  // User clicked a link: open externally via the panel's open_url path and
  // keep the view on our content.
  auto* renderer = qobject_cast<WebViewDescriptionRenderer*>(parent());
  if (renderer != nullptr)
    emit renderer->link_clicked(url);
  else
    QDesktopServices::openUrl(url);
  return false;
}

WebViewDescriptionRenderer::WebViewDescriptionRenderer(QWidget* parent)
    : DescriptionRenderer(parent),
      webview_(new QWebEngineView(this))
{
  webview_->setPage(new InterceptPage(this));
  // Untrusted remote content: no scripts.
  webview_->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled,
                                     false);
  // QWebEngineView is a QWidget: embed it directly, no window container.
  webview_->setMinimumSize(320, 200);
  webview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  // Dark page background from the start: Chromium's default is white and
  // shows through on first paint and during every setHtml swap before the
  // body CSS lands. This is what the user sees as a white flash.
  if (webview_->page() != nullptr)
    webview_->page()->setBackgroundColor(page_background(current_style_));
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(webview_, 1);
  setMinimumHeight(200);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void WebViewDescriptionRenderer::set_description(const QString& html)
{
  current_html_ = html;
  webview_->setHtml(wrap_web_html(html, current_style_));
}

void WebViewDescriptionRenderer::set_source_style(SourceCSS style)
{
  current_style_ = style;
  // Keep the page background in sync so a style switch never exposes the
  // white default underneath the new theme.
  if (webview_ != nullptr && webview_->page() != nullptr)
    webview_->page()->setBackgroundColor(page_background(current_style_));
}

void WebViewDescriptionRenderer::clear()
{
  current_html_.clear();
  webview_->setHtml(wrap_web_html(QString(), current_style_));
}

}  // namespace ui
