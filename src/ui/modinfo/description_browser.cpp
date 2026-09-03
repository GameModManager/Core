#include "ui/modinfo/description_browser.h"

// =============================================================================
// DescriptionBrowser's QNAM is a documented exception to the engine's
// "Network:: is the only gateway" rule. Reason: QTextBrowser::loadResource()
// is a synchronous QVariant-returning hook that Qt calls from the layout pass;
// the cleanest integration is a per-instance QNAM whose replies install
// QImage resources back into the document. Routing through the engine would
// add a future / a worker thread for no functional gain. We accept the
// leak in exchange for the simpler model.
//
// Future direction (out of scope here): wrap this in a tiny QObject that
// translates nam_->get(reply) into Network::request() and emits a signal
// back to DescriptionBrowser; until that lands, the grep guardrail in
// engine/network/network_manager.h documents this file as the lone
// exception.
// =============================================================================

#include <QImage>
#include <QImageReader>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTextDocument>
#include <QThreadPool>
#include <QTimer>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace ui {

namespace {

// Cap on a single image fetch. A slow / unreachable CDN should not pin
// a request indefinitely; after the timeout we drop the reply and the
// document keeps a broken-image glyph in place of the picture.
constexpr int kImageFetchTimeoutMs = 10000;

// File-decode worker: read + decode off the UI thread. Returns an empty
// QImage on any failure (missing file, malformed bytes, unsupported
// format). Lives at file scope so the tests can call it directly.
QImage decode_image_file_impl(const QString &path) {
  if (path.isEmpty())
    return {};
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QImage img;
  if (!reader.read(&img))
    return {};
  return img;
}

QImage decode_image_bytes_impl(const QByteArray &bytes) {
  if (bytes.isEmpty())
    return {};
  QImageReader reader(bytes);
  reader.setAutoTransform(true);
  QImage img;
  if (!reader.read(&img))
    return {};
  return img;
}

} // namespace

QImage decode_image_file(const QString &path) {
  return decode_image_file_impl(path);
}
QImage decode_image_bytes(const QByteArray &bytes) {
  return decode_image_bytes_impl(bytes);
}

bool desc_perf_logging_enabled() {
  static const bool enabled = []() {
    const char *v = std::getenv("GMM_DESC_PERF");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

namespace {
// stderr logger gated on GMM_DESC_PERF. Cheap, no allocations beyond
// the format buffer, only fires when the env var is set. Used during
// the SkyParkour v3 freeze investigation (Workspace-ggml).
void perf_log(const char *fmt, ...) {
  if (!desc_perf_logging_enabled())
    return;
  std::fprintf(stderr, "[gmm-desc] ");
  std::va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}
} // namespace

DescriptionBrowser::DescriptionBrowser(QWidget *parent)
    : QTextBrowser(parent), nam_(new QNetworkAccessManager(this)) {
  // openExternalLinks: matches the prior QTextBrowser usage in
  // nexus_source_panel / steam_source_panel so existing click-to-open
  // behavior is preserved.
  setOpenExternalLinks(true);
  // searchPaths defaulting to empty matches the QTextBrowser default; the
  // existing panels never set them, so neither do we.
  // Debounced re-layout: every install_decoded_image() inserts into
  // dirty_pending_ and starts a 0-ms single-shot. Subsequent inserts
  // during the same tick coalesce; the slot fires once with the full
  // set. Without this, a 30-image description would issue 30 back-to-
  // back markContentsDirty() calls and re-layout the entire document 30
  // times. With it, one layout pass covers all images that landed in
  // the same event-loop cycle.
  dirty_timer_ = new QTimer(this);
  dirty_timer_->setSingleShot(true);
  dirty_timer_->setInterval(0);
  connect(dirty_timer_, &QTimer::timeout, this,
          &DescriptionBrowser::flush_pending_dirty);
}

DescriptionBrowser::~DescriptionBrowser() {
  // In-flight replies are parented to `this` and Qt cancels them on
  // destruction; clear the bookkeeping explicitly so we don't process a
  // finished() signal from a half-torn-down object. Workers already
  // running will see their QPointer<DescriptionBrowser> go null and
  // become no-ops.
  const auto replies = in_flight_.values();
  in_flight_.clear();
  for (QNetworkReply *r : replies) {
    if (r != nullptr) {
      r->abort();
      r->deleteLater();
    }
  }
  pending_.clear();
  dirty_pending_.clear();
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
  // Forget every url we previously handed to the document; the next
  // description will rebuild its own image set. pending_ is dropped too:
  // any worker that lands later will see its QPointer null and bail.
  cached_images_.clear();
  pending_.clear();
  dirty_pending_.clear();
  if (dirty_timer_ != nullptr)
    dirty_timer_->stop();
}

void DescriptionBrowser::flush_pending_dirty() {
  if (dirty_pending_.isEmpty())
    return;
  dirty_pending_.clear();
  if (auto *doc = document()) {
    // Repaint the whole document. The browser is sized to the description
    // pane so the cost is bounded; coalescing across one event-loop tick
    // turns an N-stall burst into a single layout pass.
    doc->markContentsDirty(0, doc->characterCount());
  }
}

void DescriptionBrowser::install_decoded_image(QUrl name, QImage image) {
  if (image.isNull()) {
    pending_.remove(name);
    return;
  }
  cached_images_.insert(name, image);
  pending_.remove(name);
  if (auto *doc = document()) {
    // addResource MUST run on the UI thread. install_decoded_image is
    // always called via QMetaObject::invokeMethod with Qt::QueuedConnection
    // (or directly when the worker is somehow already on the UI thread,
    // which QtConcurrent prevents), so this stays single-threaded.
    doc->addResource(QTextDocument::ImageResource, name, QVariant(image));
    dirty_pending_.insert(name);
    if (dirty_timer_ != nullptr && !dirty_timer_->isActive())
      dirty_timer_->start();
  }
}

void DescriptionBrowser::schedule_decode(QUrl name, QByteArray bytes) {
  // Mark pending so subsequent loadResource() calls during the same
  // layout pass return an empty QVariant instead of starting a second
  // fetch / re-entering the network path.
  pending_.insert(name);

  // QPointer guards against the browser being destroyed while the
  // worker is running (tab closed mid-render). QPointer is not formally
  // thread-safe per Qt docs, so we capture it by value and ALWAYS re-
  // check for null on the worker before reading self.data() and before
  // the queued invoke on the UI thread; the browser's destroyed() runs
  // on the UI thread and the loadResource path can otherwise race the
  // worker pool completing after destruction. Once the pointer is null,
  // install_decoded_image becomes a no-op.
  QPointer<DescriptionBrowser> self(this);
  const QUrl url = name;

  auto run = [url, bytes, self]() {
    // Guard #1: the browser may already have been torn down by the time
    // we get a worker thread. Bail before any self.data() access.
    if (!self)
      return;
    QImage img;
    if (bytes.isEmpty()) {
      // file:// / qrc:// / any non-http URL: resolve the local path off
      // -thread. Doing this here means the QFile::open +
      // QString::toLocal8Bit_helper chain no longer blocks the UI
      // thread on every [img] tag the document paints.
      QString path;
      if (url.isLocalFile()) {
        path = url.toLocalFile();
      } else if (url.scheme() == QLatin1String("qrc")) {
        // toString(PreferLocalFile) returns "qrc:///foo" for qrc://
        // URLs, which QImageReader cannot open. Strip the scheme and
        // fall through to the resource-rooted path ("/foo") it
        // expects; the file may still not exist on disk, in which
        // case decode_image_file_impl returns a null QImage and the
        // pending_ entry is cleared via the failure path below.
        path = QStringLiteral(":/") + url.path();
      } else {
        path = url.toString(QUrl::PreferLocalFile);
      }
      img = decode_image_file_impl(path);
    } else {
      img = decode_image_bytes_impl(bytes);
    }
    // Always hop back to the UI thread, success OR failure: failure
    // must clear pending_ so the URL is not pinned forever and the
    // next loadResource() can retry. install_decoded_image() itself
    // bails on null images and is the single point of pending_ cleanup.
    QMetaObject::invokeMethod(
        self.data(),
        [self, url, img]() {
          // Guard #2: re-check on the UI thread before touching the
          // browser. invokeMethod(self.data(), ...) requires a non-
          // null object, and the browser may have been destroyed
          // between dispatch and the queued event firing.
          if (!self)
            return;
          self->install_decoded_image(url, img);
        },
        Qt::QueuedConnection);
  };

  // bytes == empty means we still need to resolve a local path, so the
  // worker has to run; otherwise we could skip the hop if the bytes
  // decode synchronously - we can't, because QImageReader::read is what
  // we're moving off-thread. Always dispatch.
  QThreadPool::globalInstance()->start(run);
}

QVariant DescriptionBrowser::loadResource(int type, const QUrl &name) {
  // Only ImageResource gets the async treatment; stylesheets / html
  // resources must use the parent's file-based resolution to keep
  // things predictable. Anything non-http and non-image falls through
  // to QTextBrowser's default implementation, which still does its own
  // synchronous I/O - but for non-image types that cost is negligible
  // (a single qrc:// resolve or no-op).
  if (type != QTextDocument::ImageResource) {
    return QTextBrowser::loadResource(type, name);
  }

  // Synchronous fast path: the image is already decoded and cached.
  // Returning from here avoids the layout pass that QTextBrowser does
  // on a miss (which would itself call back into loadResource). This is
  // the hot path on every subsequent layout once the first decode lands.
  if (auto it = cached_images_.constFind(name); it != cached_images_.cend()) {
    return QVariant(it.value());
  }

  const bool is_http = (name.scheme() == QLatin1String("http") ||
                        name.scheme() == QLatin1String("https"));

  // Fire a single fetch per URL. Qt deduplicates identical in-flight
  // requests via the QHash, and the reply handler below installs the
  // image into the document when it arrives.
  if (!pending_.contains(name)) {
    if (is_http) {
      if (!in_flight_.contains(name)) {
        QNetworkRequest req(name);
        req.setRawHeader("User-Agent", "GameModManager/0.4 (+description-img)");
        QNetworkReply *reply = nam_->get(req);
        in_flight_.insert(name, reply);
        // Timeout: kill the request if the CDN is slow / unreachable.
        QTimer::singleShot(kImageFetchTimeoutMs, Qt::CoarseTimer, reply,
                           [reply]() {
                             if (reply->isRunning()) {
                               reply->abort();
                             }
                           });
        connect(reply, &QNetworkReply::finished, this, [this, reply, name]() {
          in_flight_.remove(name);
          if (reply->error() != QNetworkReply::NoError) {
            pending_.remove(name);
            reply->deleteLater();
            return;
          }
          const QByteArray bytes = reply->readAll();
          reply->deleteLater();
          if (bytes.isEmpty()) {
            pending_.remove(name);
            return;
          }
          // Hand the bytes off to a worker thread for decode + addResource.
          // QImageReader::read() + the QString::toLocal8Bit_helper chain
          // it triggers on format sniffing are the synchronous stall the
          // perf trace pointed at (Workspace-ggml).
          schedule_decode(name, bytes);
        });
      }
    } else {
      // file://, qrc://, plain relative paths, etc. The QTextBrowser
      // default would do a synchronous QFile::open + QImageReader::read
      // right here on the UI thread; we instead kick the same work
      // onto the thread pool. The document paints a broken-image glyph
      // until the worker posts back, then a single batched
      // markContentsDirty swaps the picture in.
      schedule_decode(name, QByteArray());
    }
  }
  // QTextBrowser expects a synchronous answer. We have nothing cached
  // and the worker hasn't finished yet: return an invalid QVariant so
  // the layout pass paints the broken-image placeholder. The batched
  // dirty_timer_ will fire on the next event-loop tick and re-paint
  // with all the images that landed meanwhile.
  return {};
}

} // namespace ui
