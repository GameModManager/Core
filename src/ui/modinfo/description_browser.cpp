#include "ui/modinfo/description_browser.h"

#include <QImage>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextDocument>
#include <QTimer>

namespace ui {

namespace {

// Cap on a single image fetch. A slow / unreachable CDN should not pin
// a request indefinitely; after the timeout we drop the reply and the
// document keeps a broken-image glyph in place of the picture.
constexpr int kImageFetchTimeoutMs = 10000;

} // namespace

DescriptionBrowser::DescriptionBrowser(QWidget *parent)
    : QTextBrowser(parent), nam_(new QNetworkAccessManager(this)) {
  // openExternalLinks: matches the prior QTextBrowser usage in
  // nexus_source_panel / steam_source_panel so existing click-to-open
  // behavior is preserved.
  setOpenExternalLinks(true);
  // searchPaths defaulting to empty matches the QTextBrowser default; the
  // existing panels never set them, so neither do we.
}

DescriptionBrowser::~DescriptionBrowser() {
  // In-flight replies are parented to `this` and Qt cancels them on
  // destruction; clear the bookkeeping explicitly so we don't process a
  // finished() signal from a half-torn-down object.
  const auto replies = in_flight_.values();
  in_flight_.clear();
  for (QNetworkReply *r : replies) {
    if (r != nullptr) {
      r->abort();
      r->deleteLater();
    }
  }
}

void DescriptionBrowser::clear_image_cache() {
  // Cancel anything pending - the new description is unrelated and any
  // late-arriving bytes would otherwise race the document teardown.
  const auto replies = in_flight_.values();
  in_flight_.clear();
  for (QNetworkReply *r : replies) {
    if (r != nullptr) {
      r->abort();
      r->deleteLater();
    }
  }
}

QVariant DescriptionBrowser::loadResource(int type, const QUrl &name) {
  // Only ImageResource gets the network treatment; stylesheets / html
  // resources must use the parent's file-based resolution to keep
  // things predictable.
  if (type != QTextDocument::ImageResource ||
      (name.scheme() != QLatin1String("http") &&
       name.scheme() != QLatin1String("https"))) {
    return QTextBrowser::loadResource(type, name);
  }

  // Fire a single GET per URL. Qt deduplicates identical in-flight
  // requests via the QHash, and the reply handler below installs the
  // image into the document when it arrives.
  if (!in_flight_.contains(name)) {
    QNetworkRequest req(name);
    req.setRawHeader("User-Agent", "GameModManager/0.4 (+description-img)");
    QNetworkReply *reply = nam_->get(req);
    in_flight_.insert(name, reply);
    // Timeout: kill the request if the CDN is slow / unreachable.
    QTimer::singleShot(kImageFetchTimeoutMs, Qt::CoarseTimer, reply, [reply]() {
      if (reply->isRunning()) {
        reply->abort();
      }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, name]() {
      in_flight_.remove(name);
      if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return;
      }
      const QByteArray bytes = reply->readAll();
      reply->deleteLater();
      if (bytes.isEmpty())
        return;
      // QImageReader handles format sniffing (Nexus returns mixed JPEG /
      // PNG / WebP); QImage::loadFromData also works but is stricter on
      // malformed input.
      QImage img;
      QImageReader reader(bytes);
      reader.setAutoTransform(true);
      if (!reader.read(&img) || img.isNull())
        return;
      // Install into the document and trigger a repaint.
      if (auto *doc = document()) {
        doc->addResource(QTextDocument::ImageResource, name, QVariant(img));
        // The image lives inside a specific QTextImageFormat; the
        // cheapest reliable repaint is the whole document. The browser
        // is sized to the description pane so the cost is bounded.
        doc->markContentsDirty(0, doc->characterCount());
      }
    });
  }
  // QTextBrowser expects a synchronous answer; return the image if the
  // document already has it cached, otherwise return an invalid QVariant
  // (broken-image placeholder) until the network reply lands.
  if (auto *doc = document()) {
    const QVariant cached = doc->resource(QTextDocument::ImageResource, name);
    if (cached.isValid())
      return cached;
  }
  return {};
}

} // namespace ui
