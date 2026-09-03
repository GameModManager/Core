#pragma once

#include <QHash>
#include <QImage>
#include <QSet>
#include <QTextBrowser>
#include <QUrl>

#include <QByteArray>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace ui {

// QTextBrowser subclass that knows how to load http(s) image resources
// inside BBCode descriptions ([img]https://...[/img] in the Nexus / Steam
// feeds). The stock QTextBrowser::loadResource() only resolves local file
// paths and searchPaths; for a remote URL it returns an invalid QVariant
// and the document silently shows a broken image.
//
// We use a per-instance QNetworkAccessManager and a small request/reply
// cache. Loading is asynchronous end-to-end: the network GET (for
// http(s)), the on-disk read (for file://, qrc://, ...), and the
// QImageReader decode all happen off the UI thread, so a content-heavy
// description (SkyParkour v3 has dozens of high-resolution screenshots)
// no longer blocks QDialog::exec while the document paints. The
// `loadResource()` override returns immediately with a cached QImage or
// an empty QVariant (the document shows a broken-image glyph until the
// decode finishes); the worker posts back via QMetaObject::invokeMethod
// and a single QTimer::singleShot(0, ...) debounce coalesces all the
// late-arriving repaints into one re-layout. A 10 s timeout caps the
// network wait so a stalled CDN can't permanently leave a half-loaded
// image in the cache.
//
// Everything that is not http/https falls through to a file-decode path
// that runs on the QThreadPool, so the QFile::open +
// QString::toLocal8Bit_helper chain the perf trace pointed at also moves
// off the UI thread.
class DescriptionBrowser : public QTextBrowser {
  Q_OBJECT
public:
  explicit DescriptionBrowser(QWidget *parent = nullptr);
  ~DescriptionBrowser() override;

  // Drop all cached images. Called when a new description is loaded so
  // stale entries from the previous mod don't linger.
  void clear_image_cache();

protected:
  QVariant loadResource(int type, const QUrl &name) override;

private:
  // Common bookkeeping shared by the http(s) and file:// paths. `name`
  // is the canonical resource URL the document asked for. For http(s)
  // `bytes` carries the network payload; for file:// it is empty and the
  // worker reads + decodes from the local path. The thread that calls
  // `schedule_decode` is responsible for making sure the UI thread can
  // see a non-null QPointer<DescriptionBrowser> when the result lands.
  void schedule_decode(QUrl name, QByteArray bytes);
  // Called on the UI thread when a worker finishes a decode. Installs
  // the image into the document cache and schedules a single batched
  // markContentsDirty so a burst of N completions produces one re-layout.
  void install_decoded_image(QUrl name, QImage image);
  void flush_pending_dirty();

  QNetworkAccessManager *nam_ = nullptr;

  // url -> reply (so we can disconnect on cache clear / teardown).
  QHash<QUrl, QNetworkReply *> in_flight_;
  // urls whose image bytes have already been handed to the document via
  // doc->addResource(). loadResource() only consults this hash; calling
  // doc->resource() directly would dispatch back to the virtual
  // loadResource() when nothing is cached and recurse forever.
  QHash<QUrl, QImage> cached_images_;
  // urls whose bytes are in flight (network reply pending OR a worker
  // is decoding them). loadResource() returns an empty QVariant for
  // anything in here; we never start a second fetch for the same URL.
  QSet<QUrl> pending_;
  // urls whose decoded image has been handed to install_decoded_image
  // since the last markContentsDirty() flush. Coalesced into one re-
  // layout pass via a zero-delay single-shot timer.
  QSet<QUrl> dirty_pending_;
  QTimer *dirty_timer_ = nullptr;
};

// Decode a QImage from raw image bytes off the UI thread. Returns an
// empty QImage on failure (malformed input, unsupported format, etc.).
// Exposed at namespace scope so tests can call it without spinning up a
// QApplication event loop.
QImage decode_image_bytes(const QByteArray &bytes);

// Read + decode a single image from disk off the UI thread. Used for
// file:// / qrc:// resources and any non-http URL whose QTextBrowser
// would otherwise read synchronously inside loadResource(). Returns an
// empty QImage on failure.
QImage decode_image_file(const QString &path);

// Module-local env gate: set GMM_DESC_PERF=1 to print elapsed-time
// measurements (loadResource total, decode time per image, total
// install+dirty round-trip) to stderr. Off by default to keep release
// builds quiet. Used by the description-render regression investigation
// (Workspace-ggml).
bool desc_perf_logging_enabled();

} // namespace ui
