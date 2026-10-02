#include "ui/modinfo/description_renderer.h"

#include "ui/modinfo/description_browser.h"
#ifdef GMM_HAS_WEBENGINE
#include "ui/modinfo/webview_description_renderer.h"
#endif

#include <QTextBrowser>
#include <QVBoxLayout>

namespace ui {

namespace {

  // QTextBrowser shell: the BBCode pipeline emits raw '\n' newlines (libcbb
  // preserves them), so the body needs white-space:pre-wrap to show line
  // breaks. Same wrapper the panels used before the renderer split.
  QString wrap_text_html(const QString &body) {
    return QStringLiteral("<html><body style=\"font-family:sans-serif; "
                          "white-space:pre-wrap;\">"
                          "%1</body></html>")
        .arg(body);
  }

}  // namespace

void configure_chromium_flags() {
  // Chromium switches. --disable-gpu drops the GPU process along with its own
  // Vulkan use. It is not what contains the crash below, but it is free for a
  // static BBCode fragment and it removes a whole process from the picture.
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");

#ifdef Q_OS_LINUX
  // Containment. The crash is a fault in somebody else's code: a Vulkan
  // overlay that interposes vkCreateDevice (MangoHud's BGFX layer, LSFG's
  // loader shim, the Steam overlay, whatever comes next) dispatches through a
  // callback it never installed, and the process jumps to 0x0. Uninstalling
  // the overlay is not a fix - the next one lands in the same place - and
  // nothing we own controls which overlays exist on a user's machine.
  //
  // Chromium's switch surface cannot close it either. Measured against this
  // Qt 6.11 WebEngine build with VK_LOADER_DEBUG=all: --disable-gpu,
  // --use-vulkan=none, --disable-vulkan, --disable-features=Vulkan,
  // --disable-features=WebViewDrawFunctorUsesVulkan and an ANGLE/SwiftShader
  // stack each still leave one vkCreateDevice in the log with the overlay's
  // layer in the chain. Chromium is the caller, and the loader is the layer
  // we do own: with no driver manifest there is no chain to interpose and
  // vkCreateDevice cannot be reached at all (verified: zero, against one).
  //
  // Nothing legitimate is lost. GMM draws through Qt Widgets, which is a
  // raster paint engine and never a Vulkan consumer, so the only reason a
  // Vulkan device was ever created in this process was Chromium's optional
  // use of one - and Chromium's own log already reported its Vulkan
  // initialisation failing, i.e. it was not compositing with it either.
  // Chromium falls back to its GL path.
  qputenv("VK_DRIVER_FILES", "/dev/null");
#endif
}

// Applied before any Qt object exists, and on every path that can build a
// view rather than only the ones someone remembered. WebEngine reads all of
// this when it starts Chromium, which is the first QWebEngineView
// construction - and Chromium start-up was deferred to first use, so that
// first view is whichever mod's description the user happens to open, not
// app launch. A call from main() covers the app binary; this covers every
// binary that links the renderer, including tests.
[[maybe_unused]] const int g_chromium_configured = (configure_chromium_flags(), 0);

DescriptionRenderer *create_description_renderer(QWidget *parent) {
#ifdef GMM_HAS_WEBENGINE
  return new WebViewDescriptionRenderer(parent);
#else
  return new TextBrowserDescriptionRenderer(parent);
#endif
}

TextBrowserDescriptionRenderer::TextBrowserDescriptionRenderer(QWidget *parent)
    : DescriptionRenderer(parent), browser_(new DescriptionBrowser(this)) {
  // Links open externally via link_clicked so both backends share the
  // panel's data_.open_url path (matches the old setOpenExternalLinks(true)
  // behavior for the user).
  browser_->setOpenExternalLinks(false);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(browser_);
  connect(browser_, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
    emit link_clicked(url);
  });
}

void TextBrowserDescriptionRenderer::set_description(const QString &html) {
  current_ = html;
  browser_->setHtml(wrap_text_html(html));
}

void TextBrowserDescriptionRenderer::clear() {
  current_.clear();
  browser_->clear_image_cache();
  browser_->clear();
}

}  // namespace ui
