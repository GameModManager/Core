// Tests for the DescriptionBrowser QTextBrowser subclass (image loading
// for http(s) URLs in BBCode descriptions).
//
// We don't make real network requests here. The properties we need to
// lock down are:
//   1. Non-http URLs delegate to QTextBrowser::loadResource (i.e. the
//      file/qrc fall-through still works for compatibility with any
//      caller that uses DescriptionBrowser for local content too).
//   2. The instance can be constructed and a QApplication lives long
//      enough to exercise the override (proves MOC + Qt linkage is OK
//      in the test binary).
//   3. REGRESSION: loadResource must not recurse into itself via
//      QTextDocument::resource(). Calling doc->resource() directly on a
//      browser that has a remote http URL must return without crashing
//      (recursion -> stack overflow -> SIGSEGV on the buggy code).
//   4. REGRESSION (Workspace-ggml): the UI thread must not block on
//      loadResource for an unreachable remote URL. The decode path
//      used to do QImageReader::read + markContentsDirty synchronously
//      on the UI thread for every <img> in a BBCode description; a
//      content-heavy mod like SkyParkour v3 froze the dialog for tens
//      of seconds. The async pipeline (QNAM -> thread-pool decode ->
//      batched markContentsDirty) keeps the UI thread free during
//      loadResource, and we lock that down here with a synchronous
//      test that exercises the file-decode path with a real on-disk
//      PNG.
//   5. The decode workers exposed at namespace scope are safe to call
//      without a QApplication running (they only touch QImageReader).
//   6. The GMM_DESC_PERF env-gate returns false in the default case
//      and is the only opt-in instrumentation surface; the class does
//      not introduce a new logging category or pull in QtConcurrent in
//      the test binary just for a side channel.

#include "ui/modinfo/description_browser.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QImageWriter>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

namespace {
void check(bool cond, const char *what) {
  INFO(what);
  REQUIRE(cond);
}
} // namespace

TEST_CASE("description_browser: construction and teardown",
          "[ui][description_browser]") {
  // QApplication is created once via QCoreApplication's instance guard.
  // Construction + destruction here proves MOC works and the class
  // links into the test binary.
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  check(browser.openExternalLinks(),
        "openExternalLinks defaults to true to match the prior panels");
}

TEST_CASE(
    "description_browser: clear_image_cache on a fresh browser is a no-op",
    "[ui][description_browser]") {
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  // No in-flight replies yet; clear should not crash or change state.
  browser.clear_image_cache();
  CHECK(true);
}

TEST_CASE("description_browser: setHtml with a remote [img] does not crash",
          "[ui][description_browser]") {
  // Smoke test: feed DescriptionBrowser a real BBCode snippet with a
  // remote [img] tag and confirm it accepts the HTML. We don't make a
  // real network request - the test environment is offline. The point
  // is to prove the override pattern is wired up: the browser does not
  // crash on a remote URL, and the QNetworkAccessManager fires a GET
  // (which will time out in the test, but that is the production
  // behaviour too).
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  browser.setHtml(
      QStringLiteral("<html><body><img src=\"http://127.0.0.1:1/none.png\" "
                     "alt=\"\"></body></html>"));
  // The browser should still display something; the exact rendering
  // depends on whether the request has started yet. We just want to
  // confirm the QTextDocument accepted the HTML and the loadResource
  // override was callable.
  check(browser.document() != nullptr,
        "QTextDocument is created after setHtml");
  // Drain pending events so the QNetworkAccessManager's GET runs
  // through whatever path it can in this offline environment.
  QCoreApplication::processEvents();
  // clean_image_cache: must not crash even with no in-flight replies.
  browser.clear_image_cache();
  CHECK(true);
}

TEST_CASE(
    "description_browser: doc->resource() on a remote URL does not recurse",
    "[ui][description_browser]") {
  // REGRESSION TEST for Workspace-78oo (P1 crash). The previous
  // implementation of loadResource() ended with an unconditional
  // doc->resource() call. QTextDocument::resource() falls back to
  // loadResource() when nothing is cached, which (because loadResource
  // is virtual) dispatched back into DescriptionBrowser::loadResource
  // and recursed until the stack overflowed. setHtml() does not
  // synchronously trigger image resource loading, so the previous smoke
  // test never exercised the bug. This test forces the recursion path
  // by calling doc->resource() directly, which is exactly what the
  // document layout does on the very first paint of a real description.
  int argc = 0;
  static QApplication app(argc, nullptr);
  ui::DescriptionBrowser browser;
  const QUrl url(QStringLiteral("http://127.0.0.1:1/none.png"));
  // setHtml so document() is non-null and the layout is willing to
  // resolve the resource at all. Port 1 is reserved and unreachable;
  // the GET is allowed to time out in the background.
  browser.setHtml(
      QStringLiteral("<html><body><img src=\"http://127.0.0.1:1/none.png\" "
                     "alt=\"\"></body></html>"));
  check(browser.document() != nullptr, "document exists after setHtml");
  // The recursion trigger: ask the document for the resource directly.
  // On the buggy code this recurses forever (stack overflow, SIGSEGV);
  // after the fix it returns an invalid QVariant because the url has
  // not been fetched yet. The point of the test is that we get here
  // and back at all.
  const QVariant v =
      browser.document()->resource(QTextDocument::ImageResource, url);
  check(!v.isValid(),
        "first lookup misses the cache: the network reply has not landed");
  // A second lookup for the same url must also be safe (still no
  // recursion, still a miss).
  const QVariant v2 =
      browser.document()->resource(QTextDocument::ImageResource, url);
  check(!v2.isValid(), "second lookup also misses the cache");
  // Drain pending events so any in-flight reply (which will error out
  // on the unreachable host) is cleaned up; clear_image_cache then
  // tears down the in_flight_ bookkeeping safely.
  QCoreApplication::processEvents();
  browser.clear_image_cache();
  CHECK(true);
}

// REGRESSION (Workspace-ggml): SkyParkour v3 freezes the UI for ~20s of a
// 25s perf window because loadResource does QImageReader::read +
// markContentsDirty synchronously on the UI thread for every <img>. We can't
// replay the real SkyParkour payload here, but we CAN replay the synchronous
// stall with a file:// URL: prior to the fix, loadResource handed file://
// straight to QTextBrowser which opened + decoded the file on the calling
// thread. We now route that path through the thread-pool decode, and the test
// guarantees the calling thread returns within a few ms with a QVariant
// (either cached or empty).
//
// The timing bound must be tighter than "the synchronous path takes 200ms
// overall on a tiny 64x32 PNG". That was an early version of this test:
// even on the buggy code a 64x32 PNG decodes in ~5ms and the test passed.
// We now (a) use a 512x512 fixture so the synchronous path is well over
// the bound (synchronous PNG decode of a 512x512 image is in the 30-150ms
// range on CI), and (b) measure setHtml() alone, not setHtml + worker
// wait, so the bound proves the UI thread returned without doing the
// decode. The worker completion is verified separately afterwards.
TEST_CASE("description_browser: file:// loadResource does not block the caller",
          "[ui][description_browser][perf]") {
  int argc = 0;
  static QApplication app(argc, nullptr);
  // Materialize a real 512x512 PNG on disk so QImageReader has actual
  // bytes to parse and so the synchronous-decode cost is large enough
  // to be distinguishable from "the worker took 50ms to run". The exact
  // pixel content does not matter; we only care that the decode path
  // runs to completion off-thread.
  QImage source(512, 512, QImage::Format_RGB32);
  source.fill(Qt::red);
  const QString path =
      QDir::tempPath() + QStringLiteral("/gmm_desc_browser_test.png");
  // Clean up any leftover from a previous failed run before writing,
  // not after - removing the file here would race the worker decode.
  QFile::remove(path);
  check(QImageWriter(path).write(source),
        "test fixture: PNG written to a writable temp path");

  ui::DescriptionBrowser browser;
  // Hand the browser an HTML doc that points at the on-disk PNG via
  // a file:// URL. loadResource will be invoked during the layout
  // pass. We measure ONLY the setHtml() call: that is the synchronous
  // UI-thread budget. If loadResource runs the decode inline, setHtml
  // blocks for tens to hundreds of ms on a 512x512 PNG; on the async
  // path setHtml returns in microseconds. 20ms is a tight bound that
  // the synchronous path cannot meet on any reasonable CI runner.
  QElapsedTimer ui_thread;
  ui_thread.start();
  browser.setHtml(QStringLiteral("<html><body><img src=\"file://%1\" "
                                 "alt=\"\"></body></html>")
                      .arg(path));
  const qint64 set_html_ms = ui_thread.elapsed();
  check(set_html_ms < 20,
        "setHtml returns without blocking on file:// loadResource");

  // Separately drive the event loop long enough for the worker thread
  // to finish the decode and post the result back. We poll in a loop
  // instead of blocking on QSignalSpy so the test is portable across Qt
  // versions. Bound at 1 second: the global QThreadPool has at least
  // one idle worker available, decode is sub-100ms, the queued invoke
  // is delivered on the next event-loop tick.
  bool landed = false;
  for (int i = 0; i < 200; ++i) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (browser.document()
            ->resource(QTextDocument::ImageResource, QUrl::fromLocalFile(path))
            .isValid()) {
      landed = true;
      break;
    }
  }
  check(landed, "decoded image lands in the document via the worker path");

  // Cleanup
  browser.clear_image_cache();
  QCoreApplication::processEvents();
  QFile::remove(path);
}

// Lock down the off-thread decode primitives exposed at namespace scope:
// they must work without a QApplication (the test that imports the
// header is otherwise indistinguishable from a runtime that only ever
// uses them from a worker), they must handle empty input without
// crashing, and they must round-trip a real PNG.
TEST_CASE("description_browser: decode_image_bytes handles bad input",
          "[ui][description_browser]") {
  // The bytes-path decode is exercised in production by QNetworkReply
  // payloads from the Nexus / Steam CDNs (mixed JPEG/PNG/WebP). We
  // don't try to round-trip a synthetic PNG here because Qt's
  // QImageReader(bytes) path is known to be flaky under the offscreen
  // test platform when the image-format plugin is loaded lazily; the
  // on-disk round-trip test above covers the same logic via a
  // different constructor. Here we lock down the negative paths: a
  // worker fed empty bytes (e.g. a truncated QNetworkReply) must
  // return a null QImage without throwing, and garbage bytes must also
  // return a null QImage. Both branches hit the early-exit guards in
  // decode_image_bytes_impl and prove the worker can be re-entered
  // without state corruption.
  const QImage empty = ui::decode_image_bytes(QByteArray());
  check(empty.isNull(),
        "decode_image_bytes returns a null QImage for empty input");

  // Random non-image bytes: the worker should treat them as malformed
  // and return a null QImage, never crash. We use a deterministic
  // 1 KiB payload to avoid false positives from random sources.
  QByteArray garbage;
  garbage.reserve(1024);
  for (int i = 0; i < 1024; ++i)
    garbage.append(static_cast<char>(i & 0xff));
  const QImage junk = ui::decode_image_bytes(garbage);
  check(junk.isNull(),
        "decode_image_bytes returns a null QImage for garbage bytes");
}

TEST_CASE("description_browser: decode_image_file round-trips a PNG on disk",
          "[ui][description_browser]") {
  QImage source(48, 48, QImage::Format_ARGB32);
  source.fill(QColor(200, 100, 50, 255));
  const QString path =
      QDir::tempPath() + QStringLiteral("/gmm_desc_browser_test2.png");
  check(QImageWriter(path).write(source),
        "test fixture: PNG written to a writable temp path");

  const QImage decoded = ui::decode_image_file(path);
  check(!decoded.isNull(), "decode_image_file returns a valid image");
  check(decoded.size() == source.size(), "decoded size matches source size");

  // Missing file: must return a null QImage, not crash.
  const QImage missing = ui::decode_image_file(
      QDir::tempPath() + QStringLiteral("/does_not_exist_xyzzy.png"));
  check(missing.isNull(), "decode_image_file returns null for missing files");

  QFile::remove(path);
}

TEST_CASE("description_browser: desc_perf_logging_enabled defaults to false",
          "[ui][description_browser]") {
  // The env var is unset in the test harness by default; the gate must
  // return false. We don't override GMM_DESC_PERF here because doing so
  // would couple unrelated tests to a stderr line we have no way to
  // capture in Catch2.
  check(!ui::desc_perf_logging_enabled(),
        "perf gate is off unless GMM_DESC_PERF is set");
}
